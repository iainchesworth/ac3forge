#pragma once

#include <cstdint>
#include <span>
#include <vector>

#include "ac3/core/tables.hpp"

// AC-3 exponent pipeline (A/52 §7.1, §8.2.7-8.2.11).
//
// Coefficients are represented as mantissa * 2^-exponent with exponents in
// [0, 24]. The encoder extracts raw exponents from 25-bit fixed-point
// coefficients (§8.2.7: leading zeros, max 24), preprocesses them for the
// chosen strategy (§8.2.10: pairs/quads share the minimum exponent so every
// member stays representable; the absolute field is capped at 15 per §7.1.2;
// slew is limited to +-2 by only ever DECREASING exponents, which merely
// gives mantissas leading zeros and is always safe), then differentially
// encodes them three-to-a-7-bit-group (§7.1.2: 25*M1 + 5*M2 + M3).
//
// THE DECODER-MIRROR RULE (§8.2.10-8.2.11): after encoding, the encoder must
// run the normative decode (§7.1.3) and use THOSE exponents — not its raw
// ones — for mantissa normalization and bit allocation, or the decoder's
// independently computed allocation silently diverges. decode_exponents here
// is that normative §7.1.3 algorithm, shared with the future in-repo decoder.

namespace ac3 {

inline constexpr int kMaxExponent = 24;       // §8.2.7
inline constexpr int kMaxAbsoluteExponent = 15;  // 4-bit exps[ch][0] field, §7.1.2

// §7.1.3: mantissas covered by each differential exponent.
[[nodiscard]] constexpr int exponent_group_size(ExpStrategy strategy) {
    switch (strategy) {
        case ExpStrategy::kD15: return 1;
        case ExpStrategy::kD25: return 2;
        case ExpStrategy::kD45: return 4;
        case ExpStrategy::kReuse: return 0;
    }
    return 0;
}

// §7.1.3 group-count formulas (fbw channels, endmant mantissas).
[[nodiscard]] constexpr int exponent_group_count(ExpStrategy strategy, int endmant) {
    switch (strategy) {
        case ExpStrategy::kD15: return (endmant - 1) / 3;
        case ExpStrategy::kD25: return (endmant - 1 + 3) / 6;
        case ExpStrategy::kD45: return (endmant - 1 + 9) / 12;
        case ExpStrategy::kReuse: return 0;
    }
    return 0;
}

// Signed 25-bit fixed-point conversion (the float/integer seam of the
// pipeline): round(c * 2^24), clamped to the representable range.
[[nodiscard]] std::int32_t to_fixed25(double c);

// §8.2.7: leading zeros of the 24-bit magnitude, capped at 24 (zero input).
[[nodiscard]] int exponent_from_fixed(std::int32_t fixed);

// Raw exponent extraction for a whole coefficient block.
void extract_exponents(std::span<const std::int32_t> fixed, std::span<std::uint8_t> exponents);

struct EncodedExponents {
    std::uint8_t absolute = 0;             // the 4-bit exps[ch][0] field
    std::vector<std::uint8_t> groups;      // 7-bit grouped mapped values
};

// §8.2.10 encoder-side preprocessing + differential encoding. raw.size() is
// endmant; every raw exponent must be in [0, 24].
[[nodiscard]] EncodedExponents encode_exponents(std::span<const std::uint8_t> raw,
                                                ExpStrategy strategy);

// §7.1.3 normative decode: absolute + grouped values -> per-bin exponents.
// out.size() is endmant (group padding beyond endmant is discarded).
void decode_exponents(std::uint8_t absolute, std::span<const std::uint8_t> groups,
                      ExpStrategy strategy, std::span<std::uint8_t> out);

// The coupling channel's exponent set has a different shape (§7.1.3,
// §5.4.3.25): its absolute exponent is a reference that does NOT correspond
// to a coefficient - the first coded exponent is the one after it - and it is
// restricted to even values, transmitted as cplabsexp = absexp / 2. `raw`
// holds one exponent per coupling bin, and its length must be a multiple of
// 3 * the strategy's group size.
struct EncodedCouplingExponents {
    std::uint8_t cplabsexp = 0;        // the 4-bit transmitted field (absexp >> 1)
    std::vector<std::uint8_t> groups;  // ncplgrps 7-bit grouped mapped values
};

[[nodiscard]] EncodedCouplingExponents encode_coupling_exponents(
    std::span<const std::uint8_t> raw, ExpStrategy strategy);

// The matching normative decode: fills one exponent per coupling bin.
void decode_coupling_exponents(std::uint8_t cplabsexp, std::span<const std::uint8_t> groups,
                               ExpStrategy strategy, std::span<std::uint8_t> out);

}  // namespace ac3
