#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <numbers>
#include <span>
#include <vector>

#include "ac3/audio/resampler.hpp"

using ac3::audio::ClockDriftEstimator;
using ac3::audio::DriftResampler;

namespace {

// Interleaved multi-channel sine generator. `freqs` gives one frequency per
// channel, so a multi-channel case carries genuinely different, checkable
// content per channel rather than the same tone duplicated everywhere.
// `sample_rate` is purely a phase-generation parameter here - DriftResampler
// itself never sees or needs a rate, only frame counts and a ratio.
std::vector<float> generate_sine(std::size_t frames, const std::vector<double>& freqs,
                                  double sample_rate, double amplitude = 0.8,
                                  std::size_t start_frame = 0) {
    std::vector<float> out(frames * freqs.size());
    for (std::size_t n = 0; n < frames; ++n) {
        const double t = static_cast<double>(start_frame + n);
        for (std::size_t ch = 0; ch < freqs.size(); ++ch) {
            out[n * freqs.size() + ch] = static_cast<float>(
                amplitude * std::sin(2.0 * std::numbers::pi * freqs[ch] * t / sample_rate));
        }
    }
    return out;
}

std::vector<float> generate_sine_mono(std::size_t frames, double freq, double sample_rate,
                                       double amplitude = 0.8, std::size_t start_frame = 0) {
    return generate_sine(frames, {freq}, sample_rate, amplitude, start_frame);
}

// Average rising-zero-crossing period, converted to a frequency estimate.
// Cheap and robust enough for the tolerances used below without pulling in
// an FFT dependency just for tests.
double measured_frequency(std::span<const float> mono, double sample_rate) {
    std::vector<std::size_t> crossings;
    for (std::size_t i = 1; i < mono.size(); ++i) {
        if (mono[i - 1] < 0.0f && mono[i] >= 0.0f) {
            crossings.push_back(i);
        }
    }
    REQUIRE(crossings.size() >= 4);
    double total = 0.0;
    for (std::size_t i = 1; i < crossings.size(); ++i) {
        total += static_cast<double>(crossings[i] - crossings[i - 1]);
    }
    const double avg_period = total / static_cast<double>(crossings.size() - 1);
    return sample_rate / avg_period;
}

// Streaming harness mirroring the real worker's discipline: a caller-owned
// FIFO (`pending`) is topped up from `input` and drained by exactly what
// render() reports consumed, `block_frames` requested per call. Callers
// below deliberately pass odd, resampler-unrelated block sizes so a bug in
// carrying the fractional read position across call boundaries would show
// up as an error instead of being hidden by always calling render() on some
// convenient internal boundary.
std::vector<float> run_streaming(DriftResampler& resampler, std::size_t channels,
                                  const std::vector<float>& input, std::size_t total_out_frames,
                                  std::size_t block_frames) {
    std::vector<float> pending;
    std::size_t input_pos = 0;
    const std::size_t input_frames = input.size() / channels;
    std::vector<float> out;
    out.reserve(total_out_frames * channels);

    std::size_t produced = 0;
    while (produced < total_out_frames) {
        const std::size_t want = std::min(block_frames, total_out_frames - produced);
        // Top up pending so render() is never starved by the harness itself
        // (as opposed to a deliberate underrun test, which calls render()
        // directly). ratio is never below 0.5, so at most 2 input frames are
        // ever needed per output frame; this is a generous margin above
        // that.
        const std::size_t need_frames = want * 2 + 4;
        std::size_t pending_frames = pending.size() / channels;
        if (pending_frames < need_frames && input_pos < input_frames) {
            const std::size_t take = std::min(need_frames - pending_frames, input_frames - input_pos);
            pending.insert(pending.end(), input.begin() + static_cast<std::ptrdiff_t>(input_pos * channels),
                            input.begin() + static_cast<std::ptrdiff_t>((input_pos + take) * channels));
            input_pos += take;
        }
        pending_frames = pending.size() / channels;

        std::vector<float> block(want * channels);
        const std::size_t consumed = resampler.render(pending, pending_frames, block, want);
        REQUIRE(consumed <= pending_frames);
        out.insert(out.end(), block.begin(), block.end());
        pending.erase(pending.begin(), pending.begin() + static_cast<std::ptrdiff_t>(consumed * channels));
        produced += want;
    }
    return out;
}

}  // namespace

TEST_CASE("a resampler at ratio 1.0 reproduces the input", "[resampler]") {
    constexpr double kRate = 48000.0;
    constexpr std::size_t kChannels = 2;
    constexpr std::size_t kFrames = 96000;  // 2 seconds
    const auto input = generate_sine(kFrames, {440.0, 660.0}, kRate);

    DriftResampler resampler(kChannels);
    resampler.reset();
    resampler.set_ratio(1.0);
    CHECK(resampler.ratio() == 1.0);

    const auto output = run_streaming(resampler, kChannels, input, kFrames, 517);  // odd block size

    REQUIRE(output.size() == input.size());
    for (std::size_t i = 0; i < input.size(); ++i) {
        CHECK(output[i] == Catch::Approx(input[i]).margin(1e-5));
    }
}

TEST_CASE("a resampler at a real 44.1kHz->48kHz ratio preserves the tone's frequency", "[resampler]") {
    constexpr double kInRate = 44100.0;
    constexpr double kOutRate = 48000.0;
    constexpr double kRatio = kOutRate / kInRate;
    constexpr double kFreq = 1000.0;
    constexpr std::size_t kTotalOut = 144000;  // 3 seconds at 48kHz

    // ratio > 1 here, so render() always consumes fewer input frames than it
    // produces output frames - kTotalOut input frames is always enough.
    const auto input = generate_sine_mono(kTotalOut + 16, kFreq, kInRate);

    DriftResampler resampler(1);
    resampler.reset();
    resampler.set_ratio(kRatio);

    const auto output = run_streaming(resampler, 1, input, kTotalOut, 1013);  // odd block size

    // Streaming state carried correctly across many render() calls, not
    // just correct within a single call.
    REQUIRE(output.size() == kTotalOut);

    // Trim a small edge margin before measuring so any single-frame boundary
    // effect at the very start/end can't skew the zero-crossing average.
    const std::span<const float> measured_span(output.data() + 100, output.size() - 200);
    const double measured = measured_frequency(measured_span, kOutRate);
    CHECK(measured == Catch::Approx(kFreq).epsilon(0.02));
}

TEST_CASE("a resampler at a small drift ratio stays phase-locked over many frames", "[resampler]") {
    constexpr double kRate = 48000.0;
    constexpr double kRatio = 1.00002;  // ~20 ppm
    constexpr double kFreq = 1000.0;
    constexpr std::size_t kTotalOut = 240000;  // 5 seconds

    const auto input = generate_sine_mono(kTotalOut + 16, kFreq, kRate);

    DriftResampler resampler(1);
    resampler.reset();
    resampler.set_ratio(kRatio);

    const auto output = run_streaming(resampler, 1, input, kTotalOut, 977);  // odd block size
    REQUIRE(output.size() == kTotalOut);

    // Independent closed-form reference: linear interpolation over
    // in[n] = A*sin(2*pi*f*n/R) at source position k/ratio approximates
    // A*sin(2*pi*(f/ratio)*k/R) - see render()'s doc comment for the
    // src_pos derivation this mirrors. Any bug in carrying position_ across
    // call boundaries would show up as GROWING divergence from this
    // reference over the run, not just a flat per-sample interpolation
    // error.
    const double target_freq = kFreq / kRatio;
    std::vector<float> reference(output.size());
    for (std::size_t n = 0; n < reference.size(); ++n) {
        reference[n] = static_cast<float>(
            0.8 * std::sin(2.0 * std::numbers::pi * target_freq * static_cast<double>(n) / kRate));
    }

    const std::size_t window = output.size() / 10;
    auto max_abs_error = [&](std::size_t begin, std::size_t end) {
        double worst = 0.0;
        for (std::size_t n = begin; n < end; ++n) {
            worst = std::max(worst, std::abs(static_cast<double>(output[n]) - static_cast<double>(reference[n])));
        }
        return worst;
    };

    const double first_window_error = max_abs_error(0, window);
    const double last_window_error = max_abs_error(output.size() - window, output.size());
    CAPTURE(first_window_error, last_window_error);

    // Bounded: both windows sit well under the amplitude, comfortably above
    // the intrinsic linear-interpolation curvature error at this frequency
    // (on the order of 1e-3 here) but far below anything a real accumulation
    // bug would produce.
    CHECK(first_window_error < 0.02);
    CHECK(last_window_error < 0.02);
    // Not accumulating: the error at the end of a 5 second run is the same
    // order of magnitude as at the start, not growing with time.
    CHECK(last_window_error < first_window_error * 5.0 + 0.005);
}

TEST_CASE("a resampler's render pads the tail on underrun and never over-consumes", "[resampler]") {
    DriftResampler resampler(1);
    resampler.reset();
    resampler.set_ratio(1.0);

    const auto in = generate_sine_mono(10, 200.0, 48000.0, 0.8);
    std::vector<float> out(50, -999.0f);  // sentinel: an untouched slot would stand out
    const auto consumed = resampler.render(in, in.size(), out, out.size());

    CHECK(consumed <= in.size());
    CHECK(consumed == in.size());  // ratio 1.0: every available input frame gets used

    // ratio == 1.0 keeps position_ at exact integers, so the covered part is
    // a plain passthrough.
    for (std::size_t i = 0; i < in.size(); ++i) {
        CHECK(out[i] == Catch::Approx(in[i]).margin(1e-6));
    }
    // Past the end of the input, the tail holds the LAST available sample -
    // not zero, not the sentinel, not garbage.
    for (std::size_t i = in.size(); i < out.size(); ++i) {
        CHECK(out[i] == Catch::Approx(in.back()).margin(1e-6));
    }
}

TEST_CASE("a resampler's render pads with silence when there is no input at all", "[resampler]") {
    DriftResampler resampler(2);
    resampler.reset();
    resampler.set_ratio(1.0);

    std::vector<float> out(20, 1.0f);  // sentinel
    const auto consumed = resampler.render(std::span<const float>{}, 0, out, 10);

    CHECK(consumed == 0);
    for (const float sample : out) {
        CHECK(sample == 0.0f);
    }
}

TEST_CASE("a resampler's drift estimator reports zero drift before any update", "[resampler]") {
    ClockDriftEstimator estimator(1.0, 1536);
    CHECK(estimator.drift_ppm() == 0.0);
    CHECK(estimator.ratio() == Catch::Approx(1.0));
}

TEST_CASE("a resampler's drift estimator converges to a positive, bounded ppm when the FIFO runs "
          "steadily overfull",
          "[resampler]") {
    constexpr std::size_t kTarget = 2000;  // divides evenly by 20, so e lands on exactly 0.05
    ClockDriftEstimator estimator(1.0, kTarget);
    const std::size_t overfull = kTarget + kTarget / 20;  // +5% -> e = 0.05

    std::vector<double> history;
    history.reserve(400);
    for (int i = 0; i < 400; ++i) {
        estimator.update(overfull);
        history.push_back(estimator.drift_ppm());
    }

    // Steady state: c = clamp(kServoGain * e, +-kMaxCorrection)
    //             = clamp(0.02 * 0.05, +-0.002) = 0.001 (not clamped),
    // so drift_ppm settles at 1000 ppm.
    CHECK(estimator.drift_ppm() == Catch::Approx(1000.0).margin(5.0));
    // Overfull -> ratio pulled below nominal, so DriftResampler consumes
    // MORE input per fixed output count, draining the FIFO back down.
    CHECK(estimator.ratio() < 1.0);

    // Bounded and non-oscillating: well past the ~20-update settling
    // window, every remaining sample sits within a tight band of the
    // converged value.
    for (std::size_t i = 200; i < history.size(); ++i) {
        CHECK(history[i] == Catch::Approx(1000.0).margin(5.0));
    }
    // A one-pole filter chasing a constant target approaches it
    // monotonically - it never overshoots or rings.
    for (std::size_t i = 1; i < history.size(); ++i) {
        CHECK(history[i] >= history[i - 1] - 1e-9);
    }
}

TEST_CASE("a resampler's drift estimator converges to a negative ppm when the FIFO runs steadily "
          "underfull",
          "[resampler]") {
    constexpr std::size_t kTarget = 2000;  // divides evenly by 20, so e lands on exactly -0.05
    ClockDriftEstimator estimator(1.0, kTarget);
    const std::size_t underfull = kTarget - kTarget / 20;  // -5% -> e = -0.05

    std::vector<double> history;
    history.reserve(400);
    for (int i = 0; i < 400; ++i) {
        estimator.update(underfull);
        history.push_back(estimator.drift_ppm());
    }

    CHECK(estimator.drift_ppm() == Catch::Approx(-1000.0).margin(5.0));
    // Underfull -> ratio pulled above nominal, consuming input more slowly
    // so the FIFO has a chance to refill.
    CHECK(estimator.ratio() > 1.0);

    for (std::size_t i = 200; i < history.size(); ++i) {
        CHECK(history[i] == Catch::Approx(-1000.0).margin(5.0));
    }
    for (std::size_t i = 1; i < history.size(); ++i) {
        CHECK(history[i] <= history[i - 1] + 1e-9);
    }
}

TEST_CASE("a resampler and its drift estimator correct a slave clock running fast, end to end",
          "[resampler]") {
    // Two-device live session: master runs at the nominal rate; the slave's
    // hardware clock actually runs kDriftPpm fast, though nothing in this
    // loop is ever told that number directly - it only ever sees how many
    // slave frames arrived each period, exactly like the real worker.
    constexpr double kMasterRate = 48000.0;
    constexpr double kDriftPpm = 40.0;
    constexpr double kSlaveRate = kMasterRate * (1.0 + kDriftPpm * 1e-6);
    constexpr double kFreq = 300.0;
    constexpr std::size_t kFramePeriod = 1536;
    constexpr std::size_t kTargetFifo = kFramePeriod;
    constexpr std::size_t kMasterFramesTotal = 192000;  // 4 seconds
    constexpr std::size_t kPeriods = kMasterFramesTotal / kFramePeriod;
    static_assert(kPeriods * kFramePeriod == kMasterFramesTotal);

    DriftResampler resampler(1);
    resampler.reset();
    resampler.set_ratio(1.0);
    ClockDriftEstimator estimator(/*nominal_ratio=*/1.0, kTargetFifo);

    auto slave_sample = [](std::size_t n) {
        return static_cast<float>(0.8 * std::sin(2.0 * std::numbers::pi * kFreq *
                                                  static_cast<double>(n) / kSlaveRate));
    };

    // Caller-owned FIFO, pre-filled to target so the servo starts near its
    // operating point rather than from empty - a real worker pre-buffers
    // before a session starts for the same reason.
    std::vector<float> pending;
    pending.reserve(kTargetFifo * 2);
    for (std::size_t n = 0; n < kTargetFifo; ++n) {
        pending.push_back(slave_sample(n));
    }
    std::size_t slave_generated = kTargetFifo;

    std::vector<float> output;
    output.reserve(kMasterFramesTotal);

    for (std::size_t p = 0; p < kPeriods; ++p) {
        // How many slave-clock frames have arrived by this period's
        // real-time boundary, per the slave's TRUE rate. The loop only ever
        // reads off frame counts here - kSlaveRate itself is never handed
        // to the estimator or the resampler.
        const double elapsed_seconds = static_cast<double>((p + 1) * kFramePeriod) / kMasterRate;
        const auto target_slave_count = static_cast<std::size_t>(kSlaveRate * elapsed_seconds);
        for (std::size_t n = slave_generated; n < target_slave_count; ++n) {
            pending.push_back(slave_sample(n));
        }
        slave_generated = target_slave_count;

        // Drain-append, then servo, then render - the same order the real
        // worker follows (see ClockDriftEstimator::update's doc comment).
        estimator.update(pending.size());
        resampler.set_ratio(estimator.ratio());

        std::vector<float> block(kFramePeriod);
        const auto consumed = resampler.render(pending, pending.size(), block, kFramePeriod);
        pending.erase(pending.begin(), pending.begin() + static_cast<std::ptrdiff_t>(consumed));
        output.insert(output.end(), block.begin(), block.end());
    }

    REQUIRE(output.size() == kMasterFramesTotal);

    // Reference: the same tone generated directly at the master's nominal
    // rate - what a perfectly clock-locked slave would have produced.
    std::vector<float> reference(output.size());
    for (std::size_t n = 0; n < reference.size(); ++n) {
        reference[n] = static_cast<float>(
            0.8 * std::sin(2.0 * std::numbers::pi * kFreq * static_cast<double>(n) / kMasterRate));
    }

    // Skip the servo's settling window (a handful of ~20-update time
    // constants - see ClockDriftEstimator::update's ~0.6s figure) - steady-
    // state tracking is what this proves, not the startup transient. A
    // PROPORTIONAL-only servo (no integral term) needs a small persistent
    // FIFO offset from target to sustain a nonzero steady-state correction,
    // which shows up here as a small constant residual phase offset rather
    // than zero error - the thing to prove is that this residual stays
    // BOUNDED (flat) after settling, not that it vanishes.
    const std::size_t settle_frames = static_cast<std::size_t>(1.5 * kMasterRate);
    auto window_error = [&](std::size_t begin, std::size_t end) {
        double max_abs = 0.0;
        double sum_sq = 0.0;
        for (std::size_t n = begin; n < end; ++n) {
            const double err = static_cast<double>(output[n]) - static_cast<double>(reference[n]);
            max_abs = std::max(max_abs, std::abs(err));
            sum_sq += err * err;
        }
        return std::pair{max_abs, std::sqrt(sum_sq / static_cast<double>(end - begin))};
    };

    const auto [max_abs_error, rms_error] = window_error(settle_frames, output.size());
    CAPTURE(max_abs_error, rms_error);

    // Uncorrected (ratio pinned at 1.0 the whole run instead of following
    // the servo), 40 ppm of drift over 4s displaces the slave stream against
    // the master by 2*pi*kFreq*kMasterFramesTotal*kDriftPpm*1e-6/kMasterRate
    // =~ 0.30 rad by the end AND KEEPS GROWING - a max error around 0.24 and
    // still climbing by the same point in the run. A working servo instead
    // settles onto a small, flat residual well under that.
    CHECK(max_abs_error < 0.13);
    CHECK(rms_error < 0.09);

    // Bounded, not still accumulating: split the post-settle range in half
    // and check the back half is no worse than the front half (beyond a
    // generous slack for measurement noise) - an uncorrected or under-
    // corrected drift would instead keep growing across this split.
    const std::size_t mid = settle_frames + (output.size() - settle_frames) / 2;
    const double first_half_rms = window_error(settle_frames, mid).second;
    const double second_half_rms = window_error(mid, output.size()).second;
    CAPTURE(first_half_rms, second_half_rms);
    CHECK(second_half_rms < first_half_rms * 1.5 + 0.02);

    // The servo actually moved away from nominal to achieve this - proof
    // the two classes are composing, not that the ratio just sat at 1.0
    // and got lucky (true drift here is ~40 ppm).
    CHECK(estimator.drift_ppm() > 10.0);
    CHECK(estimator.drift_ppm() < 100.0);
}
