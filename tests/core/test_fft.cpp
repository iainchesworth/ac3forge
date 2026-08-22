#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cmath>
#include <numbers>

#include "ac3/core/fft.hpp"

namespace {

bool near(double a, double b, double tol = 1e-9) { return std::abs(a - b) < tol; }

}  // namespace

TEST_CASE("dft512 of a unit impulse is a flat 1/N spectrum", "[fft]") {
    // x[n] = delta[n] -> Z[k] = (1/N) * 1 for every k, the direct-form sum's
    // simplest possible check: every twiddle factor is multiplied by zero
    // except at n = 0, where cos(0) = 1 and sin(0) = 0.
    std::array<double, 512> re{};
    std::array<double, 512> im{};
    std::array<double, 512> real_out{};
    std::array<double, 512> imag_out{};
    re[0] = 1.0;
    ac3::dft512(re, im, real_out, imag_out);
    for (int k = 0; k < 512; ++k) {
        CAPTURE(k);
        CHECK(near(real_out[static_cast<std::size_t>(k)], 1.0 / 512.0));
        CHECK(near(imag_out[static_cast<std::size_t>(k)], 0.0));
    }
}

TEST_CASE("dft512 of a DC signal concentrates entirely in bin 0", "[fft]") {
    // x[n] = 1 for all n -> Z[0] = 1, Z[k != 0] = 0 (every other bin's kernel
    // sums a full period of a root of unity, which cancels exactly).
    std::array<double, 512> re{};
    std::array<double, 512> im{};
    std::array<double, 512> real_out{};
    std::array<double, 512> imag_out{};
    re.fill(1.0);
    ac3::dft512(re, im, real_out, imag_out);
    CHECK(near(real_out[0], 1.0));
    CHECK(near(imag_out[0], 0.0));
    for (int k = 1; k < 512; ++k) {
        CAPTURE(k);
        CHECK(near(real_out[static_cast<std::size_t>(k)], 0.0, 1e-8));
        CHECK(near(imag_out[static_cast<std::size_t>(k)], 0.0, 1e-8));
    }
}

TEST_CASE("dft512 of a bin-aligned real cosine splits evenly between k and N-k",
          "[fft]") {
    // x[n] = cos(2*pi*f*n/N) = 0.5*e^{j2pi f n/N} + 0.5*e^{-j2pi f n/N}, so
    // (given this DFT's e^{-j2pi kn/N} kernel) energy lands entirely at
    // k = f and k = N - f, real-valued (zero phase) at both.
    constexpr int kBin = 3;
    std::array<double, 512> re{};
    std::array<double, 512> im{};
    std::array<double, 512> real_out{};
    std::array<double, 512> imag_out{};
    for (int n = 0; n < 512; ++n) {
        re[static_cast<std::size_t>(n)] =
            std::cos(2.0 * std::numbers::pi * kBin * n / 512.0);
    }
    ac3::dft512(re, im, real_out, imag_out);
    CHECK(near(real_out[kBin], 0.5, 1e-8));
    CHECK(near(imag_out[kBin], 0.0, 1e-8));
    CHECK(near(real_out[512 - kBin], 0.5, 1e-8));
    CHECK(near(imag_out[512 - kBin], 0.0, 1e-8));
    // A bin well away from both peaks should carry negligible energy.
    CHECK(near(real_out[100], 0.0, 1e-8));
    CHECK(near(imag_out[100], 0.0, 1e-8));
}

TEST_CASE("dft512 is linear", "[fft]") {
    // Z(a*x + b*y) == a*Z(x) + b*Z(y) - a property the direct-form sum has by
    // construction, but worth pinning down since a fast (radix-2) rewrite of
    // this primitive must preserve it exactly.
    std::array<double, 512> x{};
    std::array<double, 512> y{};
    std::array<double, 512> zero{};
    x[1] = 1.0;
    y[7] = 1.0;
    std::array<double, 512> zx_re{}, zx_im{}, zy_re{}, zy_im{};
    ac3::dft512(x, zero, zx_re, zx_im);
    ac3::dft512(y, zero, zy_re, zy_im);

    std::array<double, 512> combined{};
    for (int n = 0; n < 512; ++n) {
        combined[static_cast<std::size_t>(n)] =
            2.0 * x[static_cast<std::size_t>(n)] - 0.5 * y[static_cast<std::size_t>(n)];
    }
    std::array<double, 512> zc_re{}, zc_im{};
    ac3::dft512(combined, zero, zc_re, zc_im);

    for (int k = 0; k < 512; ++k) {
        CAPTURE(k);
        const double expected_re =
            2.0 * zx_re[static_cast<std::size_t>(k)] - 0.5 * zy_re[static_cast<std::size_t>(k)];
        const double expected_im =
            2.0 * zx_im[static_cast<std::size_t>(k)] - 0.5 * zy_im[static_cast<std::size_t>(k)];
        CHECK(near(zc_re[static_cast<std::size_t>(k)], expected_re, 1e-8));
        CHECK(near(zc_im[static_cast<std::size_t>(k)], expected_im, 1e-8));
    }
}
