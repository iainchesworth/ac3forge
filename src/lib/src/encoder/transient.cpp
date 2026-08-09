#include "ac3/encoder/transient.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <numbers>

namespace ac3 {

namespace {

constexpr double kPi = std::numbers::pi;
constexpr double kCutoffHz = 8000.0;
constexpr double kQ = std::numbers::sqrt2 / 2.0;  // 1/sqrt(2), Butterworth

// §8.2.2 step 4's silence gate and per-level thresholds, verbatim.
constexpr double kSilenceThreshold = 100.0 / 32768.0;
constexpr double kT1 = 0.1;
constexpr double kT2 = 0.075;
constexpr double kT3 = 0.05;

double peak_abs(std::span<const double> x) {
    double m = 0.0;
    for (const double v : x) {
        m = std::max(m, std::abs(v));
    }
    return m;
}

}  // namespace

double TransientDetector::Biquad::process(double x) {
    const double y = b0 * x + b1 * x1 + b2 * x2 - a1 * y1 - a2 * y2;
    x2 = x1;
    x1 = x;
    y2 = y1;
    y1 = y;
    return y;
}

TransientDetector::TransientDetector(SampleRate sample_rate) {
    // RBJ audio-EQ-cookbook high-pass, both cascaded stages identical.
    const double fs = static_cast<double>(sample_rate_hz(sample_rate));
    const double w0 = 2.0 * kPi * kCutoffHz / fs;
    const double cos_w0 = std::cos(w0);
    const double alpha = std::sin(w0) / (2.0 * kQ);
    const double a0 = 1.0 + alpha;
    const Biquad stage{.b0 = (1.0 + cos_w0) / 2.0 / a0,
                       .b1 = -(1.0 + cos_w0) / a0,
                       .b2 = (1.0 + cos_w0) / 2.0 / a0,
                       .a1 = (-2.0 * cos_w0) / a0,
                       .a2 = (1.0 - alpha) / a0};
    stages_.fill(stage);
}

bool TransientDetector::run_pass(std::span<const double, 256> half) {
    std::array<double, 256> filtered{};
    for (std::size_t n = 0; n < half.size(); ++n) {
        double x = half[n];
        for (auto& stage : stages_) {
            x = stage.process(x);
        }
        filtered[n] = x;
    }

    const double p1 = peak_abs(filtered);
    const std::array<double, 2> p2{peak_abs(std::span{filtered}.subspan(0, 128)),
                                   peak_abs(std::span{filtered}.subspan(128, 128))};
    const std::array<double, 4> p3{
        peak_abs(std::span{filtered}.subspan(0, 64)),
        peak_abs(std::span{filtered}.subspan(64, 64)),
        peak_abs(std::span{filtered}.subspan(128, 64)),
        peak_abs(std::span{filtered}.subspan(192, 64)),
    };

    // §8.2.2 step 4's silence gate forces a long block regardless of the
    // ratio comparisons below. The very first pass this instance ever runs
    // has no "immediately prior tree" for P[j][0] to mean anything (the
    // spec's own carry rule is silent on that case) - comparing against the
    // default-constructed 0.0 would flag every non-silent stream's opening
    // block as a spurious transient, so it is treated the same as silence:
    // no transient, but the tree history below still advances so the NEXT
    // pass compares against real data.
    bool transient = false;
    if (!first_pass_ && p1 >= kSilenceThreshold) {
        if (p1 * kT1 > prev_level1_) {
            transient = true;
        }
        if (p2[0] * kT2 > prev_level2_) {
            transient = true;
        }
        if (p2[1] * kT2 > p2[0]) {
            transient = true;
        }
        if (p3[0] * kT3 > prev_level3_) {
            transient = true;
        }
        for (std::size_t k = 1; k < p3.size(); ++k) {
            if (p3[k] * kT3 > p3[k - 1]) {
                transient = true;
            }
        }
    }

    first_pass_ = false;
    prev_level1_ = p1;
    prev_level2_ = p2[1];
    prev_level3_ = p3.back();
    return transient;
}

bool TransientDetector::detect(std::span<const double, 512> time) {
    (void)run_pass(time.first<256>());
    return run_pass(time.last<256>());
}

}  // namespace ac3
