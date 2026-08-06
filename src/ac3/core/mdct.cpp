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

}  // namespace

void apply_analysis_window(std::span<const double, 512> x, std::span<double, 512> windowed) {
    for (int n = 0; n < kN; ++n) {
        windowed[static_cast<std::size_t>(n)] =
            x[static_cast<std::size_t>(n)] * kAnalysisWindow[static_cast<std::size_t>(n)];
    }
}

void mdct512_forward(std::span<const double, 512> windowed, std::span<double, 256> coeffs) {
    // §8.2.3.2, alpha = 0 (long transform), direct form.
    for (int k = 0; k < kN / 2; ++k) {
        double sum = 0.0;
        const double factor = 2.0 * k + 1.0;
        for (int n = 0; n < kN; ++n) {
            const double phase =
                (2.0 * kPi / (4.0 * kN)) * (2.0 * n + 1.0) * factor + (kPi / 4.0) * factor;
            sum += windowed[static_cast<std::size_t>(n)] * std::cos(phase);
        }
        coeffs[static_cast<std::size_t>(k)] = (-2.0 / kN) * sum;
    }
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

}  // namespace ac3
