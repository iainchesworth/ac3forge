#include "ac3/meta/loudness.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <numbers>
#include "ac3/core/tables.hpp"
#include <optional>
#include <span>
#include <vector>

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

// 3 s windows advanced by the same 100 ms step as above (EBU Tech 3342 §3.1:
// "a sliding analysis-window of length 3 seconds", sampled at "at least
// 10 Hz" — the existing 100 ms step already meets that exactly, so
// short-term loudness reuses it rather than running its own timer).
constexpr int kShortTermSteps = 30;

// EBU Tech 3342 §3.1's own cascaded gate for Loudness Range: the absolute
// threshold is the same -70 LUFS BS.1770 itself uses (kAbsoluteGateLkfs
// above), but the relative threshold is -20 LU below the mean of what
// survives it - NOT BS.1770's -10 LU relative gate for integrated loudness,
// and applied to a population of short-term (3 s) values rather than
// 400 ms ones. LRA is then the 95th minus the 10th percentile of what
// survives both gates ("Loudness Range: A measure to supplement EBU R 128
// loudness normalization", Tech 3342 v4 (2023), §3.1 and its MATLAB
// reference implementation in §5).
constexpr double kLraRelativeGateLu = -20.0;
constexpr double kLraLowPercentile = 10.0;
constexpr double kLraHighPercentile = 95.0;

// BS.1770 Table 3: unity for left, right and centre, +1.5 dB for each
// surround, and the LFE simply does not participate.
constexpr double kSurroundWeight = 1.41;

// ITU-R BS.1770-4 Annex 2 ("Guidelines for accurate measurement of
// 'true-peak' level"): a minimum 4x-oversampling true-peak estimator, built
// from "one set of filter coefficients (for the order 48, 4-phase, FIR
// interpolating)" the Annex tabulates verbatim. This is the standard's own
// worked example, transcribed exactly (each row below is one column of the
// Annex's table, i.e. one phase's 12 taps) rather than designed - Annex 2
// gives no formula to derive it from, only the table, the same way this
// file already transcribes Annex 1's Tables 1/2 rather than deriving them.
//
// The offline dsp::resampler used elsewhere in this project (see
// dsp/resampler.cpp) is NOT reused here: it is a whole-buffer, allocating,
// arbitrary-ratio design meant for a one-shot file conversion, and its
// windowed-sinc kernel is *designed* rather than the literal filter this
// Annex specifies. A true-peak meter instead needs a fixed, tiny,
// allocation-free per-sample kernel it can run inline in push() - exactly
// what a 12-tap/4-phase FIR with a 12-sample delay line per channel gives.
//
// The table is rate-independent by construction (Annex 2 does not tabulate
// per-rate coefficients the way the K-weighting filters above have to be
// re-derived per rate): applying it unchanged at 44.1/32 kHz still gives at
// least the 4x oversampling ratio the Annex requires as a minimum, even
// though the resulting absolute oversampled rate (176.4/128 kHz) sits a
// little under the "at least 192 kHz" figure the Annex quotes for its own
// 48 kHz worked example.
constexpr int kTruePeakTaps = 12;
constexpr int kTruePeakPhases = 4;
constexpr std::array<std::array<double, kTruePeakTaps>, kTruePeakPhases> kTruePeakCoeffs{{
    // Phase 0
    {0.0017089843750,  0.0109863281250,  -0.0196533203125, 0.0332031250000,
     -0.0594482421875, 0.1373291015625,  0.9721679687500,  -0.1022949218750,
     0.0476074218750,  -0.0266113281250, 0.0148925781250,  -0.0083007812500},
    // Phase 1
    {-0.0291748046875, 0.0292968750000,  -0.0517578125000, 0.0891113281250,
     -0.1665039062500, 0.4650878906250,  0.7797851562500,  -0.2003173828125,
     0.1015625000000,  -0.0582275390625, 0.0330810546875,  -0.0189208984375},
    // Phase 2
    {-0.0189208984375, 0.0330810546875,  -0.0582275390625, 0.1015625000000,
     -0.2003173828125, 0.7797851562500,  0.4650878906250,  -0.1665039062500,
     0.0891113281250,  -0.0517578125000, 0.0292968750000,  -0.0291748046875},
    // Phase 3
    {-0.0083007812500, 0.0148925781250,  -0.0266113281250, 0.0476074218750,
     -0.1022949218750, 0.9721679687500,  0.1373291015625,  -0.0594482421875,
     0.0332031250000,  -0.0196533203125, 0.0109863281250,  0.0017089843750},
}};

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
    short_term_recent_.assign(static_cast<std::size_t>(fullbw_), std::array<double, 30>{});
    true_peak_history_.assign(static_cast<std::size_t>(channels_), std::array<double, 12>{});
}

void LoudnessMeter::push(std::span<const std::span<const float>> channels) {
    // channels_ rather than fullbw_: true peak (below) runs over every
    // pushed channel, LFE included, so the LFE channel's own length must
    // not be dropped from the loop bound the way the K-weighting path
    // below deliberately ignores it.
    std::size_t length = 0;
    for (int ch = 0; ch < channels_ && static_cast<std::size_t>(ch) < channels.size(); ++ch) {
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
        // Separate from the K-weighting loop above: true peak measures every
        // coded channel including LFE (fullbw_ excludes it), and runs off
        // the raw sample rather than the K-weighted/filtered one - Annex 2
        // oversamples the signal itself, not a loudness-weighted version of
        // it.
        for (int ch = 0; ch < channels_ && static_cast<std::size_t>(ch) < channels.size();
             ++ch) {
            const auto& source = channels[static_cast<std::size_t>(ch)];
            push_true_peak(ch, n < source.size() ? source[n] : 0.0f);
        }
        if (++step_filled_ == step_samples_) {
            push_block();
            step_filled_ = 0;
        }
    }
}

void LoudnessMeter::push_true_peak(int channel, float sample) {
    auto& history = true_peak_history_[static_cast<std::size_t>(channel)];
    // Same left-shift/append-at-the-back convention as recent_/st_history
    // above: oldest sample drops off the front, newest lands at the back.
    // Index k is used identically by every phase below, so the particular
    // delay alignment chosen here is arbitrary and does not affect the
    // magnitude of the interpolated peak - only which output sample a given
    // input instant's energy shows up in, which this meter never reports.
    std::rotate(history.begin(), history.begin() + 1, history.end());
    history.back() = static_cast<double>(sample);

    for (int phase = 0; phase < kTruePeakPhases; ++phase) {
        const auto& taps = kTruePeakCoeffs[static_cast<std::size_t>(phase)];
        double acc = 0.0;
        for (int k = 0; k < kTruePeakTaps; ++k) {
            acc += taps[static_cast<std::size_t>(k)] * history[static_cast<std::size_t>(k)];
        }
        true_peak_abs_max_ = std::max(true_peak_abs_max_, std::abs(acc));
    }
    // The interpolated phases approximate, but do not exactly reproduce,
    // the original sample instants (the filter's passband is not perfectly
    // flat, and the delay line is still filling for the first
    // kTruePeakTaps-1 samples of the stream). Folding the raw sample into
    // the same running max costs nothing and guarantees the oversampled
    // reading is never fractionally lower than plain sample-peak would be.
    true_peak_abs_max_ = std::max(true_peak_abs_max_, std::abs(static_cast<double>(sample)));
    true_peak_seen_ = true;
}

void LoudnessMeter::push_block() {
    for (std::size_t ch = 0; ch < recent_.size(); ++ch) {
        auto& history = recent_[ch];
        history = {history[1], history[2], history[3], step_sum_[ch]};

        auto& st_history = short_term_recent_[ch];
        std::rotate(st_history.begin(), st_history.begin() + 1, st_history.end());
        st_history.back() = step_sum_[ch];

        step_sum_[ch] = 0.0;
    }
    ++steps_seen_;

    if (steps_seen_ >= kStepsPerBlock) {
        const auto samples = static_cast<double>(step_samples_) * kStepsPerBlock;
        double power = 0.0;
        for (std::size_t ch = 0; ch < recent_.size(); ++ch) {
            double sum = 0.0;
            for (const double value : recent_[ch]) {
                sum += value;
            }
            power += weights_[ch] * sum / samples;
        }
        // BS.1770-4 §2's momentary loudness is this exact block power,
        // un-gated - momentary_lkfs() reads it back directly, updated every
        // 100 ms step just like the gated series below.
        momentary_power_ = power;
        // The absolute gate is applied here rather than at the end: a block that
        // fails it can never pass the relative gate either, and dropping it now
        // keeps the stored series to one double per 100 ms of programme.
        if (power > 0.0 && kBlockOffsetDb + 10.0 * std::log10(power) > kAbsoluteGateLkfs) {
            block_power_.push_back(power);
        }
    }

    if (steps_seen_ >= kShortTermSteps) {
        const auto samples = static_cast<double>(step_samples_) * kShortTermSteps;
        double power = 0.0;
        for (std::size_t ch = 0; ch < short_term_recent_.size(); ++ch) {
            double sum = 0.0;
            for (const double value : short_term_recent_[ch]) {
                sum += value;
            }
            power += weights_[ch] * sum / samples;
        }
        short_term_power_ = power;
        // Un-gated, unlike block_power_ above: Loudness Range applies its
        // own (different) gate to this series at read time in
        // loudness_range(), so nothing is filtered out here. A block whose
        // power is exactly zero is skipped rather than stored as an
        // unrepresentable -inf LKFS - it would fail Tech 3342's -70 LUFS
        // absolute gate immediately regardless, so omitting it up front
        // changes nothing loudness_range() would have kept.
        if (power > 0.0) {
            short_term_power_history_.push_back(power);
        }
    }
}

std::optional<double> LoudnessMeter::momentary_lkfs() const {
    if (steps_seen_ < kStepsPerBlock || momentary_power_ <= 0.0) {
        return std::nullopt;
    }
    return kBlockOffsetDb + 10.0 * std::log10(momentary_power_);
}

std::optional<double> LoudnessMeter::short_term_lkfs() const {
    if (steps_seen_ < kShortTermSteps || short_term_power_ <= 0.0) {
        return std::nullopt;
    }
    return kBlockOffsetDb + 10.0 * std::log10(short_term_power_);
}

std::optional<double> LoudnessMeter::loudness_range() const {
    // Stage 1: Tech 3342's absolute gate, -70 LUFS - textually identical to
    // BS.1770's own (kAbsoluteGateLkfs), just applied to the short-term
    // series instead of 400 ms blocks.
    std::vector<double> abs_gated;
    abs_gated.reserve(short_term_power_history_.size());
    for (const double power : short_term_power_history_) {
        if (kBlockOffsetDb + 10.0 * std::log10(power) >= kAbsoluteGateLkfs) {
            abs_gated.push_back(power);
        }
    }
    if (abs_gated.empty()) {
        return std::nullopt;
    }

    // Stage 2: the relative gate, -20 LU below the mean of what survived
    // stage 1 - computed the same way integrated_lkfs() computes its own
    // (differently-thresholded) relative gate: the weights are constant, so
    // the mean of the weighted sums is the weighted sum of the means, and
    // one mean-of-power calculation stands in for re-deriving it per value.
    double sum = 0.0;
    for (const double power : abs_gated) {
        sum += power;
    }
    const double abs_gated_mean_lkfs =
        kBlockOffsetDb + 10.0 * std::log10(sum / static_cast<double>(abs_gated.size()));
    const double relative_gate = abs_gated_mean_lkfs + kLraRelativeGateLu;

    std::vector<double> levels;
    levels.reserve(abs_gated.size());
    for (const double power : abs_gated) {
        const double lkfs = kBlockOffsetDb + 10.0 * std::log10(power);
        if (lkfs >= relative_gate) {
            levels.push_back(lkfs);
        }
    }
    if (levels.empty()) {
        return std::nullopt;
    }

    // LRA is the spread of what is left, taken as a percentile range rather
    // than a min/max so one outlier block cannot single-handedly set it
    // (Tech 3342 §3.1's own rationale: a single gunshot or a fade-out should
    // not move the number). Index formula matches Tech 3342 §5's published
    // MATLAB reference exactly: round((n-1)*p/100), 0-based here versus the
    // reference's round((n-1)*p/100 + 1) 1-based - the "+1"/"-1" cancel
    // because MATLAB indexing starts at 1 where C++'s starts at 0.
    std::sort(levels.begin(), levels.end());
    const auto n = static_cast<double>(levels.size() - 1);
    const auto low_index =
        static_cast<std::size_t>(std::llround(n * kLraLowPercentile / 100.0));
    const auto high_index =
        static_cast<std::size_t>(std::llround(n * kLraHighPercentile / 100.0));
    return levels[high_index] - levels[low_index];
}

std::optional<double> LoudnessMeter::true_peak_dbtp() const {
    if (!true_peak_seen_ || true_peak_abs_max_ <= 0.0) {
        return std::nullopt;
    }
    // No 12.04 dB attenuate/compensate round trip (Annex 2 §3's steps 1 and
    // 5): that dance exists only to give fixed/integer arithmetic headroom
    // during oversampling, and this meter works in double throughout, which
    // is exactly the case Annex 2 itself says the step "is not necessary"
    // for.
    return 20.0 * std::log10(true_peak_abs_max_);
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
