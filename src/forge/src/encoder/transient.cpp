#include "ac3/encoder/transient.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <numbers>
#include "ac3/core/tables.hpp"
#include <span>

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

bool TransientDetector::run_pass(std::span<const float, 256> half) {
    std::array<double, 256> filtered{};
    for (std::size_t n = 0; n < half.size(); ++n) {
        double x = static_cast<double>(half[n]);
        for (auto& stage : stages_) {
            x = stage.process(x);
        }
        filtered[n] = x;
    }

    // One sweep of the array, at the tree's finest level - the coarser
    // levels are maxima of it (bit-identical to sweeping each level's own
    // span: max over the same values, just associated differently).
    const std::array<double, 4> p3{
        peak_abs(std::span{filtered}.subspan(0, 64)),
        peak_abs(std::span{filtered}.subspan(64, 64)),
        peak_abs(std::span{filtered}.subspan(128, 64)),
        peak_abs(std::span{filtered}.subspan(192, 64)),
    };
    const std::array<double, 2> p2{std::max(p3[0], p3[1]), std::max(p3[2], p3[3])};
    const double p1 = std::max(p2[0], p2[1]);

    // §8.2.2 step 4's silence gate forces a long block regardless of the
    // ratio comparisons below. Whether this pass's OWN result is trusted at
    // all is decided by the caller (detect()) - see its own comment for why
    // the very first block needs special handling.
    bool transient = false;
    if (p1 >= kSilenceThreshold) {
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

    prev_level1_ = p1;
    prev_level2_ = p2[1];
    prev_level3_ = p3.back();
    return transient;
}

bool TransientDetector::detect(std::span<const float, 256> pcm) {
    // The very first segment this instance ever sees has no real
    // "immediately prior tree" to compare against - its baseline is the
    // default-constructed 0.0, i.e. synthetic silence, and letting real
    // content flag against that would report a spurious transient on
    // literally any non-silent stream's opening block. The pass still runs
    // normally (so the tree history is real by the time the next segment
    // arrives), only the reported result is suppressed. Identical in effect
    // to the old full-window form's first-block suppression: that form's
    // extra first-half pass was over zero-filled history, which leaves a
    // zero-state biquad and zero peak levels exactly as constructed.
    const bool suppress = first_block_;
    first_block_ = false;
    const bool hit = run_pass(pcm);
    return !suppress && hit;
}

}  // namespace ac3
