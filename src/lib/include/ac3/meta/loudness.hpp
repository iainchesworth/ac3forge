#pragma once

#include <array>
#include <cstddef>
#include <optional>
#include <span>
#include <vector>

#include "ac3/core/tables.hpp"
#include "ac3/export.hpp"

// Programme loudness to ITU-R BS.1770-4, and the dialnorm (§5.4.2.8) that
// follows from it.
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

    [[nodiscard]] int channel_count() const { return channels_; }

   private:
    void push_block();

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
    // Weighted power sum of each gated 400 ms block. One double per 100 ms of
    // programme is all the gating needs: the weights are constant, so the mean
    // of the weighted sums equals the weighted sum of the means.
    std::vector<double> block_power_;

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
