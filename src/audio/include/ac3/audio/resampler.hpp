#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <span>

// Two-device live capture runs one WASAPI shared-mode endpoint as the
// session's clock master and a second as its slave - and shared-mode
// endpoints never share a hardware clock, even nominally-identical ones on
// the same PC, let alone a genuinely different device. Left alone, the
// slave's stream drifts against the master's a sample at a time: its FIFO
// toward the encoder either fills (slave running fast) or empties (slave
// running slow) until it over- or underruns, however good the two devices'
// individual clocks are on their own.
//
// This file is the pair of caller-driven, allocation-free pieces that
// correct that drift on the audio thread: DriftResampler applies a ratio to
// interleaved float PCM one render() call at a time, carrying only a
// fractional read position between calls; ClockDriftEstimator is the servo
// that turns a caller-owned FIFO's occupancy into the ratio DriftResampler
// should use next. Neither object owns a buffer or reads a clock itself -
// matching BasicRingBuffer's (ring_buffer.hpp) and SilenceWatchdog's
// (watchdog.hpp) own discipline of leaving ownership and timing entirely to
// the caller, so both are exercisable from a plain Catch2 test with
// synthetic data instead of a real device pair.

namespace ac3::audio {

// Streaming linear-interpolation fractional resampler. Applies a caller-
// supplied ratio (output_rate / input_rate) to interleaved float PCM,
// carrying only a fractional read position between calls - no sample data
// of its own, no allocation. See ClockDriftEstimator for where the ratio
// comes from in the two-device live session's clock-master model.
//
// Linear interpolation, not a windowed-sinc design: at the drift magnitudes
// a free-running consumer clock actually exhibits (tens of parts-per-
// million) linear interpolation's error sits far below the codec's
// psychoacoustic floor, and at a genuine nominal-rate conversion (44.1 to
// 48 kHz) it trades some high-frequency accuracy near Nyquist for an
// allocation-free, state-tiny implementation appropriate to a live capture
// hot path.
class DriftResampler {
public:
    explicit DriftResampler(std::size_t channels) : channels_(channels) {}

    // output_rate / input_rate. Clamped to [0.5, 2.0] - far wider than any
    // legal nominal conversion (32/44.1/48 kHz pairs) or realistic drift
    // needs, just a sanity backstop against a caller bug.
    void set_ratio(double output_over_input) {
        ratio_ = std::clamp(output_over_input, 0.5, 2.0);
    }

    [[nodiscard]] double ratio() const { return ratio_; }

    // Produces exactly out_frames frames into `out` (out_frames * channels
    // floats, interleaved) by linear interpolation over `in` (in_frames *
    // channels floats, interleaved). Returns the number of INPUT frames
    // actually consumed (<= in_frames) - the caller drops that many frames
    // from the FRONT of its own scratch buffer before the next render()
    // call; this object carries only the fractional remainder of the read
    // position, not any sample data. Pads the tail of `out` with the last
    // available input sample (or silence, if in_frames == 0) when `in` runs
    // out before out_frames worth has been produced - a genuine underrun,
    // which the caller is expected to count itself (this has no counter of
    // its own - see CaptureStats/PassthroughStats' own discipline of
    // counting at the call site, not inside a stateless-per-call helper).
    std::size_t render(std::span<const float> in, std::size_t in_frames,
                       std::span<float> out, std::size_t out_frames) {
        // set_ratio() clamps away from zero already; this guard is only
        // against ratio_ somehow being left non-positive (e.g. a future
        // caller that pokes it some other way), so the division below can
        // never produce inf/nan.
        const double ratio = ratio_ > 0.0 ? ratio_ : 1.0;

        for (std::size_t k = 0; k < out_frames; ++k) {
            // Fractional position, in SOURCE frames, that output frame k
            // reads from. position_ carries whatever fraction of a source
            // frame was left over after the previous render() call.
            const double src_pos = position_ + static_cast<double>(k) / ratio;
            const auto idx0 = static_cast<std::size_t>(std::floor(src_pos));
            const double frac = src_pos - static_cast<double>(idx0);

            if (idx0 >= in_frames) {
                // Already past the end of this call's input - a genuine
                // underrun. Hold the last available sample so the tail is a
                // flat continuation rather than reading garbage; with no
                // input at all this call, fall back to silence.
                for (std::size_t ch = 0; ch < channels_; ++ch) {
                    out[k * channels_ + ch] =
                        in_frames > 0 ? in[(in_frames - 1) * channels_ + ch] : 0.0f;
                }
                continue;
            }

            const std::size_t idx1 = idx0 + 1;
            if (idx1 >= in_frames) {
                // The upper tap would read one past the end. Duplicate idx0
                // for both taps rather than touching in[idx1] out of bounds
                // - equivalent to `frac` going unused for this one boundary
                // frame.
                for (std::size_t ch = 0; ch < channels_; ++ch) {
                    out[k * channels_ + ch] = in[idx0 * channels_ + ch];
                }
                continue;
            }

            for (std::size_t ch = 0; ch < channels_; ++ch) {
                const auto lower = static_cast<double>(in[idx0 * channels_ + ch]);
                const auto upper = static_cast<double>(in[idx1 * channels_ + ch]);
                out[k * channels_ + ch] =
                    static_cast<float>(lower * (1.0 - frac) + upper * frac);
            }
        }

        // Where the read position ends up after out_frames worth of output,
        // still in the SOURCE-frame domain of this call's `in`. Whatever
        // whole frames of `in` that covers is what the caller must drop;
        // the rest is the fractional remainder carried into next time,
        // relative to what will be index 0 of the caller's next buffer once
        // it has dropped exactly that many frames from the front.
        const double src_pos_end = position_ + static_cast<double>(out_frames) / ratio;
        const auto consumed =
            std::min(in_frames, static_cast<std::size_t>(std::floor(src_pos_end)));
        position_ = src_pos_end - static_cast<double>(consumed);
        return consumed;
    }

    // Zeroes the fractional read position - call once, at session start (or
    // after a device reopen), so the first render() does not carry over a
    // phase left by a previous, unrelated stream.
    void reset() { position_ = 0.0; }

private:
    std::size_t channels_;
    double ratio_ = 1.0;
    double position_ = 0.0;  // fractional input-frame offset into the NEXT render() call's `in`
};

// The servo that decides DriftResampler's ratio: a small proportional
// controller steering a caller-owned FIFO's occupancy back to a target
// level, smoothed with a one-pole filter so the ratio moves in small,
// audio-safe steps instead of jumping. The FIFO itself belongs to the
// caller (the worker's own scratch buffer draining a slave Capture's ring
// buffer) - this only ever sees frame counts, matching SilenceWatchdog's
// own caller-drives-the-clock discipline in watchdog.hpp.
class ClockDriftEstimator {
public:
    // `nominal_ratio` is the CONVERSION ratio alone (master_rate /
    // slave_rate; 1.0 when both devices run the same nominal rate) - what
    // ratio() returns before any drift correction rides on top.
    // `target_fifo_frames` is the FIFO occupancy this servo steers towards -
    // one output frame period's worth is the natural choice (enough
    // headroom to absorb one iteration's jitter without starving).
    ClockDriftEstimator(double nominal_ratio, std::size_t target_fifo_frames)
        : nominal_ratio_(nominal_ratio), target_fifo_frames_(target_fifo_frames) {}

    // Call once per frame period with the FIFO's CURRENT occupancy in
    // frames, after that iteration's drain-append and before render().
    // Updates the internal smoothed correction.
    //
    // Normalized error e = (current - target) / target: positive means the
    // FIFO is overfull (the slave is arriving faster than it is being
    // drained, i.e. the slave's clock is running fast relative to the
    // master). That error is scaled by kServoGain and clamped to
    // +-kMaxCorrection to get an instantaneous correction target c, then
    // folded into smoothed_correction_ with a one-pole filter (kSmoothing)
    // so a step change in FIFO level takes roughly 1/kSmoothing = 20
    // update() calls - about 20 frame periods, ~0.6s at 48 kHz/1536-sample
    // frames - to fully settle, keeping the ratio's motion audio-safe
    // instead of jumping frame to frame.
    void update(std::size_t current_fifo_frames) {
        // target_fifo_frames_ == 0 would otherwise divide by zero; a caller
        // that does that has a bug, and this just refuses to turn it into
        // inf/nan.
        const double target =
            static_cast<double>(target_fifo_frames_ > 0 ? target_fifo_frames_ : 1);
        const double e = (static_cast<double>(current_fifo_frames) - target) / target;
        const double c = std::clamp(kServoGain * e, -kMaxCorrection, kMaxCorrection);
        smoothed_correction_ += kSmoothing * (c - smoothed_correction_);
        has_update_ = true;
    }

    // nominal_ratio * (1 - smoothed_correction) - hand this straight to
    // DriftResampler::set_ratio every frame period.
    //
    // Sign check: an overfull FIFO gives positive e, hence positive
    // smoothed_correction_, hence a ratio BELOW nominal. Since
    // DriftResampler::render() consumes roughly out_frames / ratio input
    // frames for a fixed out_frames, a below-nominal ratio consumes MORE
    // input per call - which drains the overfull FIFO. That is the whole
    // point of the servo; getting this sign backwards would make it push
    // the FIFO further away from target instead of back towards it.
    [[nodiscard]] double ratio() const { return nominal_ratio_ * (1.0 - smoothed_correction_); }

    // The correction alone, as signed parts-per-million relative to
    // nominal - what a live session's drift readout displays ("slave
    // -18 ppm"). Exactly 0 before update() has ever been called (no data
    // yet reports no correction, not a fabricated one).
    [[nodiscard]] double drift_ppm() const { return has_update_ ? smoothed_correction_ * 1e6 : 0.0; }

private:
    // Proportional gain applied to the normalized FIFO error each update().
    static constexpr double kServoGain = 0.02;
    // Ceiling on the instantaneous (pre-smoothing) correction: 2000 ppm is
    // generous headroom against a caller passing a wild FIFO reading, not a
    // normal operating point - real consumer clock drift runs tens of ppm.
    static constexpr double kMaxCorrection = 0.002;
    // One-pole smoothing coefficient; see update()'s doc comment for the
    // resulting settling time.
    static constexpr double kSmoothing = 0.05;

    double nominal_ratio_;
    std::size_t target_fifo_frames_;
    double smoothed_correction_ = 0.0;
    bool has_update_ = false;
};

}  // namespace ac3::audio
