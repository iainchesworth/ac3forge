#pragma once

#include <array>
#include <cstdint>
#include <span>
#include <vector>

#include "ac3/export.hpp"

// MLP's lossless prediction loop - the "de-correlator" of the AES papers,
// transcribed from WO 96/37048 (whose processes US 7,193,538 B2 says the
// MLP lossless cores follow in preferred embodiments) and the JAES 2004
// paper's Figs. 10/11.
//
// Structure (WO Figs. 6a/6b, encoder transfer function (1+A)/(1+B)): the
// encoder forms, per sample, an exact fractional combination of PAST inputs
// through FIR filter A and PAST outputs through FIR filter B, quantizes it
// to an integer, and adds it to the current input:
//
//     y(i) = x(i) + Q( sum_j a[j]*x(i-1-j) - sum_j b[j]*y(i-1-j) )
//
// The decoder computes the SAME quantized value from the same (already
// reconstructed) history and subtracts:  x(i) = y(i) - Q(...). Because both
// sides quantize the same finite-precision value with the same rule, the
// round trip is bit-exact BY CONSTRUCTION, whatever the coefficients - the
// same lifting-scheme argument as matrix.hpp's PMQ cascade, applied across
// time instead of across channels. This is exactly why the WO's design
// works on any hardware: "As the input and output signals are both
// quantized and filters A and B are both FIR, the input to the quantizer Q
// is a finite-precision signal, and the quantization can therefore be
// specified precisely" (JAES 2004, Sec. 4.4).
//
// Coefficients are integer numerators over 2^shift, per the WO: "these six
// coefficients a1, a2, a3, b1, b2, b3, will all be of the form m/16 or
// m/64" (shift 4 or 6; the WO's Table 1 presets use quarters, shift 2),
// with ranges like -192 <= 64*a1 <= 192 bounding the m/64 case. Orders up
// to 8 per the JAES paper ("IIR or FIR filters up to eighth order").
//
// Deliberately NOT decided here (format-layer details, see
// docs/concepts/truehd-mlp.md): the exact rounding rule of the shipping
// quantizer, and the exact loop topology variant (WO Figs. 6c/6d place the
// input add after the quantizer). We use round-half-away-from-zero
// (matrix.hpp's quantize) and the pre-quantizer-add form; both choices are
// internally lossless regardless, and reconciling them to the shipping
// convention is a layer-3/4 task.

namespace ac3::mlp {

inline constexpr int kMaxPredictorOrder = 8;

struct PredictorCoefficients {
    // Denominator exponent: coefficients are numerator / 2^shift. WO forms:
    // 4 (m/16) or 6 (m/64); Table 1's presets are quarters (2).
    int shift = 6;
    // Feedforward (A) and feedback (B) numerators, a[0] multiplying the
    // most recent past sample. Sizes are the filter orders, each <= 8.
    std::vector<std::int32_t> a;
    std::vector<std::int32_t> b;
};

// The filter memories - the WO's per-block "initialisation data": "suitable
// state variables for a filter with n'th order denominator and numerator
// are the first n input samples and the first n output samples". history[0]
// is the most recent sample. A restart point seeds these from transmitted
// data; block-to-block continuation just keeps the same state running.
struct PredictorState {
    std::array<std::int64_t, kMaxPredictorOrder> input{};   // past x
    std::array<std::int64_t, kMaxPredictorOrder> output{};  // past y

    void reset() {
        input.fill(0);
        output.fill(0);
    }
};

// Encode `input` to residuals in `output` (same length), advancing `state`.
AC3FORGE_EXPORT void predict_encode(const PredictorCoefficients& coefficients,
                                    std::span<const std::int32_t> input,
                                    std::span<std::int32_t> output, PredictorState& state);

// The exact inverse: residuals back to samples, advancing `state`
// identically. decode(encode(x)) == x bit-for-bit given equal initial
// states.
AC3FORGE_EXPORT void predict_decode(const PredictorCoefficients& coefficients,
                                    std::span<const std::int32_t> residual,
                                    std::span<std::int32_t> output, PredictorState& state);

// WO Table 1: eight preset second-order coefficient cases (quarters,
// a3 = b3 = 0) given for 44.1 kHz material - transcribed as-is, useful as
// known-good stable IIR test vectors and as a starter palette before a real
// coefficient-selection strategy exists. Case 0 is the trivial passthrough.
[[nodiscard]] AC3FORGE_EXPORT PredictorCoefficients table1_preset(int case_index);
inline constexpr int kTable1Cases = 8;

}  // namespace ac3::mlp
