#include "ac3/core/mdct.hpp"

#include <array>
#include <cmath>
#include <numbers>

#include "ac3/core/window.hpp"

namespace ac3 {

namespace {

constexpr int kN = kTransformLength;  // 512
constexpr double kPi = std::numbers::pi;

// §7.9.4.1 step 2: xcos1[k] = -cos(2pi(8k+1)/8N), xsin1[k] = -sin(2pi(8k+1)/8N).
struct Twiddles {
    std::array<double, kN / 4> cos1;
    std::array<double, kN / 4> sin1;
    Twiddles() {
        for (int k = 0; k < kN / 4; ++k) {
            const double angle = 2.0 * kPi * (8.0 * k + 1.0) / (8.0 * kN);
            cos1[static_cast<std::size_t>(k)] = -std::cos(angle);
            sin1[static_cast<std::size_t>(k)] = -std::sin(angle);
        }
    }
};

const Twiddles& twiddles() {
    static const Twiddles t;
    return t;
}

// §7.9.4.2 step 2: xcos2[k] = -cos(2pi(8k+1)/4N), xsin2[k] = -sin(2pi(8k+1)/4N)
// (N = 512 throughout this section, per the spec's own note — these are NOT
// the 256-sample transform's own N).
struct Twiddles2 {
    std::array<double, kN / 8> cos2;
    std::array<double, kN / 8> sin2;
    Twiddles2() {
        for (int k = 0; k < kN / 8; ++k) {
            const double angle = 2.0 * kPi * (8.0 * k + 1.0) / (4.0 * kN);
            cos2[static_cast<std::size_t>(k)] = -std::cos(angle);
            sin2[static_cast<std::size_t>(k)] = -std::sin(angle);
        }
    }
};

const Twiddles2& twiddles2() {
    static const Twiddles2 t;
    return t;
}

// §8.2.3.2 direct form, generalized over the transform length and alpha:
// alpha = 0/N=512 is the long transform; alpha = -1/+1 at N=256 are the two
// halves of a block-switched block.
void mdct_forward_core(std::span<const double> windowed, int n_len, double alpha,
                        std::span<double> coeffs) {
    for (int k = 0; k < n_len / 2; ++k) {
        double sum = 0.0;
        const double factor = 2.0 * k + 1.0;
        for (int n = 0; n < n_len; ++n) {
            const double phase = (2.0 * kPi / (4.0 * n_len)) * (2.0 * n + 1.0) * factor +
                                  (kPi / 4.0) * factor * (1.0 + alpha);
            sum += windowed[static_cast<std::size_t>(n)] * std::cos(phase);
        }
        coeffs[static_cast<std::size_t>(k)] = (-2.0 / n_len) * sum;
    }
}

}  // namespace

void apply_analysis_window(std::span<const double, 512> x, std::span<double, 512> windowed) {
    for (int n = 0; n < kN; ++n) {
        windowed[static_cast<std::size_t>(n)] =
            x[static_cast<std::size_t>(n)] * kAnalysisWindow[static_cast<std::size_t>(n)];
    }
}

void mdct512_forward(std::span<const double, 512> windowed, std::span<double, 256> coeffs) {
    mdct_forward_core(windowed, kN, 0.0, coeffs);
}

void mdct256_forward_first(std::span<const double, 256> windowed, std::span<double, 128> coeffs) {
    mdct_forward_core(windowed, 256, -1.0, coeffs);
}

void mdct256_forward_second(std::span<const double, 256> windowed, std::span<double, 128> coeffs) {
    mdct_forward_core(windowed, 256, 1.0, coeffs);
}

void imdct512_windowed(std::span<const double, 256> coeffs, std::span<double, 512> x) {
    const auto& tw = twiddles();
    constexpr int kQuarter = kN / 4;  // 128
    constexpr int kEighth = kN / 8;   // 64

    // Step 2: pre-transform complex multiply.
    // Z[k] = (X[N/2-2k-1] + j*X[2k]) * (xcos1[k] + j*xsin1[k])
    std::array<double, kQuarter> z_re{};
    std::array<double, kQuarter> z_im{};
    for (int k = 0; k < kQuarter; ++k) {
        const double a = coeffs[static_cast<std::size_t>(kN / 2 - 2 * k - 1)];
        const double b = coeffs[static_cast<std::size_t>(2 * k)];
        const double c = tw.cos1[static_cast<std::size_t>(k)];
        const double s = tw.sin1[static_cast<std::size_t>(k)];
        z_re[static_cast<std::size_t>(k)] = a * c - b * s;
        z_im[static_cast<std::size_t>(k)] = b * c + a * s;
    }

    // Step 3: N/4-point complex "IFFT" exactly as the pseudocode sums it:
    // z[n] = sum_k Z[k] * (cos(8*pi*k*n/N) + j*sin(8*pi*k*n/N)), no scaling.
    std::array<double, kQuarter> t_re{};
    std::array<double, kQuarter> t_im{};
    for (int n = 0; n < kQuarter; ++n) {
        double re = 0.0;
        double im = 0.0;
        for (int k = 0; k < kQuarter; ++k) {
            const double angle = 8.0 * kPi * k * n / kN;
            const double c = std::cos(angle);
            const double s = std::sin(angle);
            re += z_re[static_cast<std::size_t>(k)] * c - z_im[static_cast<std::size_t>(k)] * s;
            im += z_re[static_cast<std::size_t>(k)] * s + z_im[static_cast<std::size_t>(k)] * c;
        }
        t_re[static_cast<std::size_t>(n)] = re;
        t_im[static_cast<std::size_t>(n)] = im;
    }

    // Step 4: post-transform complex multiply. y[n] = z[n] * (xcos1[n] + j*xsin1[n])
    std::array<double, kQuarter> y_re{};
    std::array<double, kQuarter> y_im{};
    for (int n = 0; n < kQuarter; ++n) {
        const double c = tw.cos1[static_cast<std::size_t>(n)];
        const double s = tw.sin1[static_cast<std::size_t>(n)];
        y_re[static_cast<std::size_t>(n)] =
            t_re[static_cast<std::size_t>(n)] * c - t_im[static_cast<std::size_t>(n)] * s;
        y_im[static_cast<std::size_t>(n)] =
            t_im[static_cast<std::size_t>(n)] * c + t_re[static_cast<std::size_t>(n)] * s;
    }

    // Step 5: windowing and de-interleaving, transcribed field-for-field.
    const auto& w = kAnalysisWindow;
    const auto yr = [&](int i) { return y_re[static_cast<std::size_t>(i)]; };
    const auto yi = [&](int i) { return y_im[static_cast<std::size_t>(i)]; };
    for (int n = 0; n < kEighth; ++n) {
        x[static_cast<std::size_t>(2 * n)] = -yi(kEighth + n) * w[static_cast<std::size_t>(2 * n)];
        x[static_cast<std::size_t>(2 * n + 1)] =
            yr(kEighth - n - 1) * w[static_cast<std::size_t>(2 * n + 1)];
        x[static_cast<std::size_t>(kQuarter + 2 * n)] =
            -yr(n) * w[static_cast<std::size_t>(kQuarter + 2 * n)];
        x[static_cast<std::size_t>(kQuarter + 2 * n + 1)] =
            yi(kQuarter - n - 1) * w[static_cast<std::size_t>(kQuarter + 2 * n + 1)];
        x[static_cast<std::size_t>(kN / 2 + 2 * n)] =
            -yr(kEighth + n) * w[static_cast<std::size_t>(kN / 2 - 2 * n - 1)];
        x[static_cast<std::size_t>(kN / 2 + 2 * n + 1)] =
            yi(kEighth - n - 1) * w[static_cast<std::size_t>(kN / 2 - 2 * n - 2)];
        x[static_cast<std::size_t>(3 * kN / 4 + 2 * n)] =
            yi(n) * w[static_cast<std::size_t>(kQuarter - 2 * n - 1)];
        x[static_cast<std::size_t>(3 * kN / 4 + 2 * n + 1)] =
            -yr(kQuarter - n - 1) * w[static_cast<std::size_t>(kQuarter - 2 * n - 2)];
    }
}

void imdct256_pair_windowed(std::span<const double, 256> coeffs, std::span<double, 512> x) {
    const auto& tw = twiddles2();
    constexpr int kQuarter = kN / 4;  // 128
    constexpr int kEighth = kN / 8;   // 64

    // Step 1: de-interleave the 256 coefficients into the two half-block sets.
    std::array<double, kQuarter> x1{};
    std::array<double, kQuarter> x2{};
    for (int k = 0; k < kQuarter; ++k) {
        x1[static_cast<std::size_t>(k)] = coeffs[static_cast<std::size_t>(2 * k)];
        x2[static_cast<std::size_t>(k)] = coeffs[static_cast<std::size_t>(2 * k + 1)];
    }

    // Step 2: pre-IFFT complex multiply.
    // Z1[k] = (X1[N/4-2k-1] + j*X1[2k]) * (xcos2[k] + j*xsin2[k]), likewise Z2.
    std::array<double, kEighth> z1_re{};
    std::array<double, kEighth> z1_im{};
    std::array<double, kEighth> z2_re{};
    std::array<double, kEighth> z2_im{};
    for (int k = 0; k < kEighth; ++k) {
        const double c = tw.cos2[static_cast<std::size_t>(k)];
        const double s = tw.sin2[static_cast<std::size_t>(k)];
        const double a1 = x1[static_cast<std::size_t>(kQuarter - 2 * k - 1)];
        const double b1 = x1[static_cast<std::size_t>(2 * k)];
        z1_re[static_cast<std::size_t>(k)] = a1 * c - b1 * s;
        z1_im[static_cast<std::size_t>(k)] = b1 * c + a1 * s;
        const double a2 = x2[static_cast<std::size_t>(kQuarter - 2 * k - 1)];
        const double b2 = x2[static_cast<std::size_t>(2 * k)];
        z2_re[static_cast<std::size_t>(k)] = a2 * c - b2 * s;
        z2_im[static_cast<std::size_t>(k)] = b2 * c + a2 * s;
    }

    // Step 3: two independent N/8-point complex "IFFT" sums, unscaled.
    std::array<double, kEighth> t1_re{};
    std::array<double, kEighth> t1_im{};
    std::array<double, kEighth> t2_re{};
    std::array<double, kEighth> t2_im{};
    for (int n = 0; n < kEighth; ++n) {
        double re1 = 0.0;
        double im1 = 0.0;
        double re2 = 0.0;
        double im2 = 0.0;
        for (int k = 0; k < kEighth; ++k) {
            const double angle = 16.0 * kPi * k * n / kN;
            const double c = std::cos(angle);
            const double s = std::sin(angle);
            re1 += z1_re[static_cast<std::size_t>(k)] * c - z1_im[static_cast<std::size_t>(k)] * s;
            im1 += z1_re[static_cast<std::size_t>(k)] * s + z1_im[static_cast<std::size_t>(k)] * c;
            re2 += z2_re[static_cast<std::size_t>(k)] * c - z2_im[static_cast<std::size_t>(k)] * s;
            im2 += z2_re[static_cast<std::size_t>(k)] * s + z2_im[static_cast<std::size_t>(k)] * c;
        }
        t1_re[static_cast<std::size_t>(n)] = re1;
        t1_im[static_cast<std::size_t>(n)] = im1;
        t2_re[static_cast<std::size_t>(n)] = re2;
        t2_im[static_cast<std::size_t>(n)] = im2;
    }

    // Step 4: post-IFFT complex multiply. y1[n] = z1[n] * (xcos2[n] + j*xsin2[n]).
    std::array<double, kEighth> y1_re{};
    std::array<double, kEighth> y1_im{};
    std::array<double, kEighth> y2_re{};
    std::array<double, kEighth> y2_im{};
    for (int n = 0; n < kEighth; ++n) {
        const double c = tw.cos2[static_cast<std::size_t>(n)];
        const double s = tw.sin2[static_cast<std::size_t>(n)];
        y1_re[static_cast<std::size_t>(n)] =
            t1_re[static_cast<std::size_t>(n)] * c - t1_im[static_cast<std::size_t>(n)] * s;
        y1_im[static_cast<std::size_t>(n)] =
            t1_im[static_cast<std::size_t>(n)] * c + t1_re[static_cast<std::size_t>(n)] * s;
        y2_re[static_cast<std::size_t>(n)] =
            t2_re[static_cast<std::size_t>(n)] * c - t2_im[static_cast<std::size_t>(n)] * s;
        y2_im[static_cast<std::size_t>(n)] =
            t2_im[static_cast<std::size_t>(n)] * c + t2_re[static_cast<std::size_t>(n)] * s;
    }

    // Step 5: windowing and de-interleaving, transcribed field-for-field.
    // N is 512 throughout (the spec's own note), so this reaches the same
    // full x[0..511] the long path's step 5 does.
    const auto& w = kAnalysisWindow;
    const auto y1r = [&](int i) { return y1_re[static_cast<std::size_t>(i)]; };
    const auto y1i = [&](int i) { return y1_im[static_cast<std::size_t>(i)]; };
    const auto y2r = [&](int i) { return y2_re[static_cast<std::size_t>(i)]; };
    const auto y2i = [&](int i) { return y2_im[static_cast<std::size_t>(i)]; };
    for (int n = 0; n < kEighth; ++n) {
        x[static_cast<std::size_t>(2 * n)] = -y1i(n) * w[static_cast<std::size_t>(2 * n)];
        x[static_cast<std::size_t>(2 * n + 1)] =
            y1r(kEighth - n - 1) * w[static_cast<std::size_t>(2 * n + 1)];
        x[static_cast<std::size_t>(kQuarter + 2 * n)] =
            -y1r(n) * w[static_cast<std::size_t>(kQuarter + 2 * n)];
        x[static_cast<std::size_t>(kQuarter + 2 * n + 1)] =
            y1i(kEighth - n - 1) * w[static_cast<std::size_t>(kQuarter + 2 * n + 1)];
        x[static_cast<std::size_t>(kN / 2 + 2 * n)] =
            -y2r(n) * w[static_cast<std::size_t>(kN / 2 - 2 * n - 1)];
        x[static_cast<std::size_t>(kN / 2 + 2 * n + 1)] =
            y2i(kEighth - n - 1) * w[static_cast<std::size_t>(kN / 2 - 2 * n - 2)];
        x[static_cast<std::size_t>(3 * kN / 4 + 2 * n)] =
            y2i(n) * w[static_cast<std::size_t>(kQuarter - 2 * n - 1)];
        x[static_cast<std::size_t>(3 * kN / 4 + 2 * n + 1)] =
            -y2r(kEighth - n - 1) * w[static_cast<std::size_t>(kQuarter - 2 * n - 2)];
    }
}

}  // namespace ac3
