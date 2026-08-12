#include "ac3/core/fft.hpp"

#include <array>
#include <cmath>
#include <numbers>

namespace ac3 {

namespace {

constexpr int kN = kDftLength;

// cos(2*pi*m/N) and sin(2*pi*m/N) for m = 0..N-1. Every (k, n) pair in the
// direct-form sum below needs the angle for (k*n) mod N, which lands in this
// same table - so N trig calls cover every one of the N^2 terms instead of
// one per term.
struct Twiddles {
    std::array<double, kN> cos{};
    std::array<double, kN> sin{};
    Twiddles() {
        constexpr double kPi = std::numbers::pi;
        for (int m = 0; m < kN; ++m) {
            const double angle = 2.0 * kPi * static_cast<double>(m) / static_cast<double>(kN);
            cos[static_cast<std::size_t>(m)] = std::cos(angle);
            sin[static_cast<std::size_t>(m)] = std::sin(angle);
        }
    }
};

const Twiddles& twiddles() {
    static const Twiddles t;
    return t;
}

}  // namespace

void dft512(std::span<const double, kDftLength> real_in,
           std::span<const double, kDftLength> imag_in, std::span<double, kDftLength> real_out,
           std::span<double, kDftLength> imag_out) {
    const auto& tw = twiddles();
    for (int k = 0; k < kN; ++k) {
        double sum_re = 0.0;
        double sum_im = 0.0;
        for (int n = 0; n < kN; ++n) {
            const int idx = (k * n) % kN;
            const double c = tw.cos[static_cast<std::size_t>(idx)];
            const double s = tw.sin[static_cast<std::size_t>(idx)];
            const double a = real_in[static_cast<std::size_t>(n)];
            const double b = imag_in[static_cast<std::size_t>(n)];
            // Z[k] = (1/N) sum_n (a + jb)(cos(2*pi*k*n/N) - j*sin(2*pi*k*n/N))
            sum_re += a * c + b * s;
            sum_im += b * c - a * s;
        }
        real_out[static_cast<std::size_t>(k)] = sum_re / static_cast<double>(kN);
        imag_out[static_cast<std::size_t>(k)] = sum_im / static_cast<double>(kN);
    }
}

}  // namespace ac3
