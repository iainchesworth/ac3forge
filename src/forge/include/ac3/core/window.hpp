#pragma once

#include <array>
#include <cstddef>
#include <numbers>

// The AC-3 transform window (A/52 §8.2.3.1, Table 7.33): a 512-point
// symmetric window given in the standard as 256 values used back-to-back.
// The published values are the Kaiser-Bessel-derived (KBD) window with
// alpha = 5 (Kaiser kernel beta = 5*pi, kernel length N/2 + 1 = 257) —
// verified: rounding this construction to 5 decimals reproduces every
// Table 7.33 entry exactly (see tools/generators/gen_mdct_goldens.py).
//
// Generated at compile time from the formula; unit tests pin it against the
// Table 7.33 values and an independent numpy evaluation. The custom Bessel
// and sqrt routines exist because constexpr <cmath> lands only in C++26.

namespace ac3 {

inline constexpr int kTransformLength = 512;  // long-transform N

namespace detail {

constexpr double constexpr_sqrt(double x) {
    if (x <= 0.0) {
        return 0.0;
    }
    double guess = x < 1.0 ? 1.0 : x;
    for (int i = 0; i < 64; ++i) {
        const double next = 0.5 * (guess + x / guess);
        if (next == guess) {
            break;
        }
        guess = next;
    }
    return guess;
}

// Modified Bessel function of the first kind, order zero:
// I0(x) = sum_k ((x/2)^k / k!)^2. Terms shrink fast for the arguments used
// here (<= 5*pi); 200 iterations is far past double convergence.
constexpr double bessel_i0(double x) {
    const double half_x = x / 2.0;
    double term = 1.0;
    double sum = 1.0;
    for (int k = 1; k < 200; ++k) {
        term *= (half_x / k) * (half_x / k);
        sum += term;
        if (term < sum * 1e-19) {
            break;
        }
    }
    return sum;
}

consteval std::array<double, kTransformLength> make_analysis_window() {
    constexpr int kKernelLength = kTransformLength / 2 + 1;  // 257
    constexpr double kBeta = 5.0 * std::numbers::pi;

    std::array<double, kKernelLength> kernel{};
    for (int i = 0; i < kKernelLength; ++i) {
        const double t = 2.0 * i / (kKernelLength - 1) - 1.0;
        kernel[static_cast<std::size_t>(i)] =
            bessel_i0(kBeta * constexpr_sqrt(1.0 - t * t)) / bessel_i0(kBeta);
    }

    double total = 0.0;
    for (const double k : kernel) {
        total += k;
    }

    std::array<double, kTransformLength> window{};
    double cumulative = 0.0;
    for (int n = 0; n < kTransformLength / 2; ++n) {
        cumulative += kernel[static_cast<std::size_t>(n)];
        const double value = constexpr_sqrt(cumulative / total);
        window[static_cast<std::size_t>(n)] = value;
        window[static_cast<std::size_t>(kTransformLength - 1 - n)] = value;
    }
    return window;
}

}  // namespace detail

// w[n], n = 0..511. w[n] == w[511-n] (§8.2.3.1: 256 coefficients used
// back-to-back to form a 512-point symmetrical window).
inline constexpr auto kAnalysisWindow = detail::make_analysis_window();

}  // namespace ac3
