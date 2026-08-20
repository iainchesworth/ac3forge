#pragma once

#include <array>
#include <cmath>
#include <cstdint>
#include <numbers>
#include <span>
#include <utility>

// The iterative radix-2 decimation-in-time FFT core shared by the §7.9.4
// fast MDCT fold (mdct.cpp, P = 128) and dft512 (fft.cpp, P = 512, the
// enhanced-coupling spectrum both encoder and decoder consume). Extracted
// from mdct.cpp's fast path verbatim - same tables, same butterfly loop,
// same evaluation order - so hoisting it here changed nothing numerically
// for the MDCT; dft512's move from its direct-form sum onto this core is
// that file's own, separately-verified change.
//
// Internal to src/forge/src/core/ on purpose - transform plumbing between
// translation units, not library surface.

namespace ac3::internal {

// One-time tables for a P-point transform: everything angle-dependent,
// computed once - the same treatment Twiddles/InnerSumTable/ForwardCosTable
// give every other transform in this library. The stage table stores
// std::cos/std::sin of each exact angle -2*pi*j/len rather than generating
// twiddles by iterated complex multiply (which carries j-1 accumulated
// rounding steps by its j-th butterfly).
template <std::size_t P>
struct FftRadix2Tables {
    static_assert((P & (P - 1)) == 0 && P >= 2, "radix-2 needs a power of two");
    // Bit-reversal permutation of 0..P-1 for the decimation-in-time FFT.
    std::array<std::uint16_t, P> bitrev{};
    // Stage twiddles exp(-2*pi*i*j/len) for len = 2, 4, ..., P and
    // j < len/2, flattened at offset len/2 - 1: stage `len` holds len/2
    // entries, so the stages pack exactly into P - 1 slots.
    std::array<double, P - 1> stage_re{};
    std::array<double, P - 1> stage_im{};
    FftRadix2Tables() {
        for (std::size_t i = 1; i < P; ++i) {
            bitrev[i] = static_cast<std::uint16_t>(
                (bitrev[i >> 1] >> 1) | ((i & 1) != 0 ? P / 2 : 0));
        }
        for (std::size_t len = 2; len <= P; len <<= 1) {
            const std::size_t half = len / 2;
            for (std::size_t j = 0; j < half; ++j) {
                const double ang = -2.0 * std::numbers::pi * static_cast<double>(j) /
                                   static_cast<double>(len);
                stage_re[half - 1 + j] = std::cos(ang);
                stage_im[half - 1 + j] = std::sin(ang);
            }
        }
    }
};

// In place over separate re/im arrays: on return (re, im) hold
// A[k] = sum_m a[m] * exp(-2*pi*i*m*k/P) for k = 0..P-1 (unnormalized
// forward transform). Split arrays rather than std::complex so the
// butterfly's four independent multiply-add chains stay visible to the
// auto-vectorizer.
template <std::size_t P>
void fft_radix2_forward(const FftRadix2Tables<P>& t, std::span<double, P> re,
                        std::span<double, P> im) {
    for (std::size_t i = 1; i < P; ++i) {
        const std::size_t j = t.bitrev[i];
        if (i < j) {
            std::swap(re[i], re[j]);
            std::swap(im[i], im[j]);
        }
    }
    for (std::size_t len = 2; len <= P; len <<= 1) {
        const std::size_t half = len / 2;
        for (std::size_t i = 0; i < P; i += len) {
            for (std::size_t j = 0; j < half; ++j) {
                const double wr = t.stage_re[half - 1 + j];
                const double wi = t.stage_im[half - 1 + j];
                const double xr = re[i + j + half];
                const double xi = im[i + j + half];
                const double vr = xr * wr - xi * wi;
                const double vi = xr * wi + xi * wr;
                const double ur = re[i + j];
                const double ui = im[i + j];
                re[i + j] = ur + vr;
                im[i + j] = ui + vi;
                re[i + j + half] = ur - vr;
                im[i + j + half] = ui - vi;
            }
        }
    }
}

}  // namespace ac3::internal
