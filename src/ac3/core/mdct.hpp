#pragma once

#include <span>

// The AC-3 long (512-sample) transform pair.
//
// Forward (encoder side, informative A/52 §8.2.3.2, alpha = 0):
//   XD[k] = (-2/N) * sum_{n=0}^{N-1} x[n] * cos((2pi/4N)(2n+1)(2k+1)
//                                              + (pi/4)(2k+1))
// evaluated in direct form — correctness first; the §7.9.4 fast N/4-FFT
// structure comes later behind the same interface.
//
// Inverse (decoder side, NORMATIVE §7.9.4.1): the N/4-point complex
// transform with xcos1/xsin1 pre/post twiddles and the windowing/
// de-interleaving map, transcribed exactly from the pseudocode. The forward
// transform is validated by round-tripping through this normative inverse
// plus the §7.9.4.1 step-6 overlap-add (pcm = 2 * (x + delay), the factor
// of 2 undoing encoder headroom scaling).

namespace ac3 {

// Multiply a raw 512-sample block by the analysis window (§8.2.3.1).
void apply_analysis_window(std::span<const double, 512> x, std::span<double, 512> windowed);

// Forward MDCT of a pre-windowed block: 512 samples -> 256 coefficients.
void mdct512_forward(std::span<const double, 512> windowed, std::span<double, 256> coeffs);

// Normative inverse: 256 coefficients -> 512 WINDOWED time samples
// (§7.9.4.1 steps 1-5; the window application is part of step 5).
// Reconstruction: pcm[n] = 2 * (x[n] + previous_block_x[256 + n]).
void imdct512_windowed(std::span<const double, 256> coeffs, std::span<double, 512> x);

}  // namespace ac3
