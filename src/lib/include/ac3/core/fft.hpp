#pragma once

#include <span>

#include "ac3/export.hpp"

// A general N=512-point complex DFT, unrelated to the AC-3 MDCT/IMDCT pair in
// mdct.hpp: enhanced coupling's decode algorithm (A/52:2018 §E3.5.5.1 step 5)
// needs the FULL complex spectrum of a windowed, overlap-added coupling
// channel signal, not the MDCT's N/4 real transform. Direct form only, matching
// this project's stance on mdct.hpp's forward transform: correctness first,
// transcribed exactly from the spec's own summation
//
//   Z[k] = (1/N) * sum_{n=0}^{N-1} (x_re[n] + j.x_im[n]) *
//                                  (cos(2*pi*k*n/N) - j.sin(2*pi*k*n/N))
//
// A fast (radix-2) structure behind the same interface is a later concern,
// once there is a decoder round-trip to validate it against.

namespace ac3 {

inline constexpr int kDftLength = 512;

AC3FORGE_EXPORT void dft512(std::span<const double, kDftLength> real_in,
                            std::span<const double, kDftLength> imag_in,
                            std::span<double, kDftLength> real_out,
                            std::span<double, kDftLength> imag_out);

}  // namespace ac3
