#pragma once

#include <span>

#include "ac3/export.hpp"

// The AC-3 long (512-sample) transform pair.
//
// Forward (encoder side, informative A/52 §8.2.3.2, alpha = 0):
//   XD[k] = (-2/N) * sum_{n=0}^{N-1} x[n] * cos((2pi/4N)(2n+1)(2k+1)
//                                              + (pi/4)(2k+1))
// evaluated in direct form by default — at THIS level the reference form
// stays the default, because the direct evaluation is the spec's own
// statement of the transform and the oracle every fast-path test validates
// against. `fast` selects the §7.9.4 fast N/4-FFT structure behind this same
// interface: verified max relative error ~3e-12 against the direct form on
// random data and real audio (tests/core/test_mdct_fast.cpp), and since the owner
// accepted that evidence it is what every encoder config defaults to
// (EncoderConfig::fast_mdct / eac3::FrameConfig::fast_mdct default true and
// are what an encoder actually reads to decide - a caller of THIS function
// still opts in explicitly). Only THIS transform (alpha = 0) has an
// accelerated path today - mdct256_forward_first/second's `fast` currently
// has no effect; see their own doc comment for why.
//
// Inverse (decoder side, NORMATIVE §7.9.4.1): the N/4-point complex
// transform with xcos1/xsin1 pre/post twiddles and the windowing/
// de-interleaving map, transcribed exactly from the pseudocode. The forward
// transform is validated by round-tripping through this normative inverse
// plus the §7.9.4.1 step-6 overlap-add (pcm = 2 * (x + delay), the factor
// of 2 undoing encoder headroom scaling).
//
// The inverses' `fast` selects a radix-2 FFT for step 3's N/4-point complex
// sum ONLY - the spec's own pseudocode evaluates that step as a direct
// O(N^2) sum against a tabulated (k, n) matrix, and with the +j*sin sign
// convention it is exactly an unscaled inverse DFT, so the fast path is
// conj(FFT(conj(Z))) through the same radix-2 core the forward's fast fold
// already uses (fft_radix2.hpp). Steps 2, 4 and 5 - the normative twiddles
// and the windowing/de-interleave map - are the identical code either way.
// Direct remains the default at THIS level for the forward's own reason:
// the direct evaluation is the spec's statement of the transform and the
// oracle the fast path's tests validate against. DecoderConfig::fast_imdct
// is what a decoder actually reads to decide.

namespace ac3 {

// Multiply a raw 512-sample block by the analysis window (§8.2.3.1).
AC3FORGE_EXPORT void apply_analysis_window(std::span<const double, 512> x,
                                           std::span<double, 512> windowed);

// Forward MDCT of a pre-windowed block: 512 samples -> 256 coefficients.
AC3FORGE_EXPORT void mdct512_forward(std::span<const double, 512> windowed,
                                     std::span<double, 256> coeffs, bool fast = false);

// Normative inverse: 256 coefficients -> 512 WINDOWED time samples
// (§7.9.4.1 steps 1-5; the window application is part of step 5).
// Reconstruction: pcm[n] = 2 * (x[n] + previous_block_x[256 + n]).
AC3FORGE_EXPORT void imdct512_windowed(std::span<const double, 256> coeffs,
                                       std::span<double, 512> x, bool fast = false);

// The block-switched (short) transform pair (§7.9, blksw = 1): the usual
// 512-sample windowed block split into two 256-sample halves, each
// transformed separately with alpha = -1 (first) or +1 (second, §8.2.3.2).
// Each half yields 128 coefficients; per §7.9.2 the encoder interleaves
// them bin-by-bin (X[2k] = first[k], X[2k+1] = second[k]) into an ordinary
// 256-coefficient set before quantization — exponents/bitalloc/mantissa
// never see a difference from the long-block path.
//
// Both halves accelerate behind the same `fast` parameter mdct512_forward
// takes, each with its OWN independently-derived fold (alpha = -1 is the
// BARE cosine sum - no "+N/4" phase shift at all, a different transform
// from alpha = 0's, not the same formula "just at a different NLen" as an
// earlier version of this comment wrongly claimed; alpha = +1 is a
// sine-kernel sum that reaches the shared DCT-IV core through the DST-IV
// reversal identity). Each fold is verified against its own direct-form
// table to the same 1e-10 bound as the long transform's -
// tests/core/test_mdct_fast.cpp. `fast = false` remains the spec's own
// direct-form evaluation on all three.
AC3FORGE_EXPORT void mdct256_forward_first(std::span<const double, 256> windowed,
                                           std::span<double, 128> coeffs, bool fast = false);
AC3FORGE_EXPORT void mdct256_forward_second(std::span<const double, 256> windowed,
                                            std::span<double, 128> coeffs, bool fast = false);

// Normative inverse for blksw = 1 (§7.9.4.2): takes the SAME 256-length
// interleaved coefficient set a long block would carry and produces 512
// WINDOWED time samples, using the identical overlap-add as
// imdct512_windowed — callers do not need to know which transform path ran.
AC3FORGE_EXPORT void imdct256_pair_windowed(std::span<const double, 256> coeffs,
                                            std::span<double, 512> x, bool fast = false);

}  // namespace ac3
