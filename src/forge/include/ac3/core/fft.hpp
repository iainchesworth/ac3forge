#pragma once

#include <span>

#include "ac3/export.hpp"

// A general N=512-point complex DFT, unrelated to the AC-3 MDCT/IMDCT pair in
// mdct.hpp: enhanced coupling's decode algorithm (A/52:2018 §E3.5.5.1 step 5)
// needs the FULL complex spectrum of a windowed, overlap-added coupling
// channel signal, not the MDCT's N/4 real transform. Computes the spec's own
// summation
//
//   Z[k] = (1/N) * sum_{n=0}^{N-1} (x_re[n] + j.x_im[n]) *
//                                  (cos(2*pi*k*n/N) - j.sin(2*pi*k*n/N))
//
// via the shared radix-2 core (src/forge/src/core/fft_radix2.hpp - the same
// machinery the §7.9.4 fast MDCT runs at P = 128). It began as the
// direct-form O(N^2) sum on this project's correctness-first stance, with
// the fast structure deferred "once there is a decoder round-trip to
// validate it against" - that round-trip exists now (the encoder/decoder
// ecpl legs of tools/ci/quality_race.py, plus this transform's own property
// tests), and the FFT holds those to tighter error than the direct form
// did. The output spans must not alias the inputs (never legal here, even
// in the direct form).

namespace ac3 {

inline constexpr int kDftLength = 512;

AC3FORGE_EXPORT void dft512(std::span<const double, kDftLength> real_in,
                            std::span<const double, kDftLength> imag_in,
                            std::span<double, kDftLength> real_out,
                            std::span<double, kDftLength> imag_out);

}  // namespace ac3
