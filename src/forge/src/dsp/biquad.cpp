#include "ac3/dsp/biquad.hpp"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace ac3::dsp {

void Biquad::set_coefficients(double b0, double b1, double b2, double a1, double a2) {
    b0_ = b0;
    b1_ = b1;
    b2_ = b2;
    a1_ = a1;
    a2_ = a2;
}

float Biquad::process(float sample) {
    const double x = static_cast<double>(sample);
    // Direct Form II Transposed: y[n] = b0*x[n] + z1[n-1], with z1/z2
    // updated from y rather than from past x/y samples directly - the
    // structure that keeps only two state variables (as opposed to Direct
    // Form I's four: x[n-1], x[n-2], y[n-1], y[n-2]) while remaining
    // numerically well-behaved for the kind of low-Q, low-corner-frequency
    // coefficients a 120 Hz low-pass produces.
    const double y = b0_ * x + z1_;
    z1_ = b1_ * x - a1_ * y + z2_;
    z2_ = b2_ * x - a2_ * y;
    return static_cast<float>(y);
}

void Biquad::reset() {
    z1_ = 0.0;
    z2_ = 0.0;
}

namespace {

// 4th-order Butterworth pole angles (N=4): splitting the transfer function
// into two second-order sections puts one complex-conjugate pole pair in
// each, at angles (2k-1)*pi/(2N) from the negative real axis for k = 1, 2;
// each pair's equivalent Q is 1 / (2*cos(angle)). That gives two distinct Q
// values rather than one repeated - the shape that makes the cascade a
// genuine maximally-flat Butterworth response instead of two identical,
// non-Butterworth 2nd-order low-passes stacked together. Computed once,
// stated as literals so the constructor has no runtime trig beyond what
// each stage's own corner-frequency coefficients need.
constexpr double kQStage1 = 0.5411961001461969;  // 1 / (2*cos(pi/8)),   k=1
constexpr double kQStage2 = 1.3065629648763766;  // 1 / (2*cos(3*pi/8)), k=2

// A coded stream's sample rate is always many multiples of a 120 Hz LFE
// corner in this project (32/44.1/48 kHz and their E-AC-3 half-rates all
// clear this with enormous headroom), so this bound is a backstop against a
// caller bug or a future reuse of this class with a much higher corner, not
// a normal operating limit. Held well below Nyquist (rather than right up
// against it) because the RBJ bilinear-transform formula's frequency
// warping grows without bound as the corner approaches Nyquist, and at
// Nyquist itself sin(omega) hits zero, degenerating alpha (and with it Q's
// entire effect on the response) to nothing.
constexpr double kMaxCornerFractionOfNyquist = 0.9;
constexpr double kMinCornerHz = 1.0;

// RBJ Audio EQ Cookbook low-pass biquad, normalized so a0 == 1. Textbook
// bilinear-transform derivation (public-domain DSP, not sourced from any
// particular codebase): omega is the corner expressed as a fraction of the
// sampling rate in radians, alpha is the half-bandwidth term that Q shapes,
// and b0/b1/b2/a1/a2 fall straight out of the standard low-pass biquad
// formulas before dividing through by a0.
void configure_lowpass(Biquad& biquad, double corner_hz, double sample_rate, double q) {
    const double omega = 2.0 * std::numbers::pi * corner_hz / sample_rate;
    const double cos_omega = std::cos(omega);
    const double sin_omega = std::sin(omega);
    const double alpha = sin_omega / (2.0 * q);

    const double b0 = (1.0 - cos_omega) / 2.0;
    const double b1 = 1.0 - cos_omega;
    const double b2 = (1.0 - cos_omega) / 2.0;
    const double a0 = 1.0 + alpha;
    const double a1 = -2.0 * cos_omega;
    const double a2 = 1.0 - alpha;

    biquad.set_coefficients(b0 / a0, b1 / a0, b2 / a0, a1 / a0, a2 / a0);
}

}  // namespace

LfeLowpass::LfeLowpass(double corner_hz, std::uint32_t sample_rate) {
    // sample_rate == 0 would otherwise divide by zero inside
    // configure_lowpass; a caller that does that has a bug, and this just
    // refuses to turn it into NaN/inf rather than trying to encode anything
    // meaningful for a filter with no defined sampling rate.
    const double safe_rate = sample_rate > 0 ? static_cast<double>(sample_rate) : 1.0;
    const double nyquist = safe_rate / 2.0;
    const double max_corner = std::max(nyquist * kMaxCornerFractionOfNyquist, kMinCornerHz);
    const double safe_corner = std::clamp(corner_hz, kMinCornerHz, max_corner);

    configure_lowpass(stage1_, safe_corner, safe_rate, kQStage1);
    configure_lowpass(stage2_, safe_corner, safe_rate, kQStage2);
}

void LfeLowpass::process(std::span<float> samples) {
    for (float& sample : samples) {
        sample = stage2_.process(stage1_.process(sample));
    }
}

void LfeLowpass::reset() {
    stage1_.reset();
    stage2_.reset();
}

}  // namespace ac3::dsp
