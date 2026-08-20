#pragma once

#include <array>
#include <cstddef>
#include <optional>
#include <span>
#include <vector>

#include "ac3/core/tables.hpp"
#include "ac3/export.hpp"

// Programme loudness to ITU-R BS.1770-4, and the dialnorm (§5.4.2.8) that
// follows from it. Also the rest of an R128-style meter built on the same
// K-weighted, channel-summed signal: momentary/short-term loudness
// (BS.1770-4 §2's un-gated block power at two window sizes), Loudness Range
// (EBU Tech 3342's own gated-percentile statistic) and true-peak level
// (BS.1770-4 Annex 2's oversampled peak, independent of the loudness path
// entirely). Roadmap item C1.
//
// A/52 defines dialnorm as how far "the average dialogue level is below
// digital 100 percent" and never says how to measure it — the standard is
// from 1995 and BS.1770 is from 2006, so in the spec's own era the answer was
// a trained listener with a level meter. Every modern delivery specification
// that fills the gap names BS.1770 gated loudness (ATSC A/85, EBU R 128), so
// that is what this encoder measures, and dialnorm is the negated integrated
// loudness rounded to the nearest dB. For material with no dialogue at all —
// music, effects — the integrated programme loudness IS the anchor A/85 asks
// for, so one measurement serves both cases.
//
// The measurement is inherently two-pass: the relative gate needs the whole
// programme before any block's contribution is known. A streaming encoder
// therefore cannot derive dialnorm from the frame it is encoding; the caller
// measures first and configures the encoder second, which is exactly what
// real encoders do with an analysis pass.

namespace ac3::meta {

class AC3FORGE_EXPORT LoudnessMeter {
   public:
    // Channel weights follow BS.1770 Table 3: unity for the front channels,
    // +1.5 dB for the surrounds, and the LFE excluded outright.
    LoudnessMeter(SampleRate rate, Acmod acmod, bool lfe);

    // Any number of samples; spans are the coded channels in AC-3 order with
    // LFE last, matching the encoder's own input convention.
    void push(std::span<const std::span<const float>> channels);

    // std::nullopt until at least one 400 ms block has passed the absolute
    // gate — silence has no meaningful loudness, and inventing one would put
    // a wrong dialnorm on the stream.
    [[nodiscard]] std::optional<double> integrated_lkfs() const;

    // BS.1770-4 §2's un-gated block loudness: the same 400 ms/75%-overlap
    // window integrated_lkfs() gates internally, reported directly instead.
    // Reflects the most recently completed 400 ms block; std::nullopt until
    // one has elapsed. A block whose power is exactly zero is also
    // std::nullopt rather than -inf LKFS, matching integrated_lkfs()'s own
    // "no meaningful loudness" stance on silence.
    [[nodiscard]] std::optional<double> momentary_lkfs() const;

    // The same, over a 3 s window instead of 400 ms, still un-gated.
    // std::nullopt until 3 s have elapsed.
    [[nodiscard]] std::optional<double> short_term_lkfs() const;

    // EBU Tech 3342 §3.1 Loudness Range: the 95th minus the 10th percentile
    // of short-term loudness values, themselves passed through Tech 3342's
    // own cascaded gate — an absolute threshold at −70 LUFS then a relative
    // one at −20 LU below the mean of what survives it. This is NOT the same
    // relative gate integrated_lkfs() uses (−10 LU): Tech 3342 §3.1 specifies
    // −20 LU for LRA specifically, and the population being gated is
    // short-term (3 s) blocks rather than integrated-loudness (400 ms) ones.
    // std::nullopt until at least one short-term value survives both gates.
    [[nodiscard]] std::optional<double> loudness_range() const;

    // ITU-R BS.1770-4 Annex 2: the highest absolute sample value found in a
    // 4x-oversampled reconstruction of every pushed channel, LFE included —
    // true peak is about physical overload headroom, not perceived
    // loudness, so unlike every measure above it does not exclude LFE or
    // apply the surround weighting. In dBTP (decibels relative to 100% full
    // scale, true-peak measurement). std::nullopt until at least one sample
    // has been pushed.
    [[nodiscard]] std::optional<double> true_peak_dbtp() const;

    [[nodiscard]] int channel_count() const { return channels_; }

   private:
    void push_block();
    void push_true_peak(int channel, float sample);

    // BS.1770 K-weighting: a high-shelf pre-filter then the RLB high-pass,
    // both biquads, both per channel with their own state.
    struct Biquad {
        std::array<double, 3> b{};
        std::array<double, 2> a{};  // a0 normalised out
    };
    struct State {
        std::array<double, 2> x{};
        std::array<double, 2> y{};
    };

    Biquad shelf_{};
    Biquad highpass_{};
    std::vector<State> shelf_state_;
    std::vector<State> highpass_state_;
    std::vector<double> weights_;
    // Mean-square accumulator per channel over the current 100 ms step, plus
    // the four most recent steps, which is how the 400 ms window with 75%
    // overlap is built without buffering audio.
    std::vector<double> step_sum_;
    std::vector<std::array<double, 4>> recent_;
    // Same idea as recent_, widened to a 3 s/30-step window for short-term
    // loudness and (via short_term_power_history_ below) Loudness Range.
    // EBU Tech 3342 §3.1 asks for at least 10 Hz sampling of the short-term
    // series; the existing 100 ms step already gives exactly that, so no
    // separate timer is needed.
    std::vector<std::array<double, 30>> short_term_recent_;
    // Weighted power sum of each gated 400 ms block. One double per 100 ms of
    // programme is all the gating needs: the weights are constant, so the mean
    // of the weighted sums equals the weighted sum of the means.
    std::vector<double> block_power_;
    // The whole-programme series of un-gated short-term (3 s) block power,
    // one entry per 100 ms once the first 3 s has elapsed — loudness_range()
    // applies Tech 3342's own gate to this at read time, since it is a
    // different gate to the one block_power_ was already filtered through.
    std::vector<double> short_term_power_history_;
    double momentary_power_ = 0.0;
    double short_term_power_ = 0.0;

    // Per-channel delay line for the true-peak oversampler (BS.1770-4
    // Annex 2), sized to channels_ (LFE included) rather than fullbw_.
    std::vector<std::array<double, 12>> true_peak_history_;
    double true_peak_abs_max_ = 0.0;
    bool true_peak_seen_ = false;

    int channels_ = 0;
    int fullbw_ = 0;
    int step_samples_ = 0;
    int step_filled_ = 0;
    int steps_seen_ = 0;
};

// §5.4.2.8: dialnorm is how many dB dialogue sits below digital 100%, valid
// 1..31. A programme louder than −1 LKFS or quieter than −31 clamps; the
// clamp at 31 is why a stream that never measured anything says 31, and why
// 31 is a poor default rather than a neutral one.
[[nodiscard]] AC3FORGE_EXPORT int dialnorm_from_lkfs(double lkfs);

}  // namespace ac3::meta
