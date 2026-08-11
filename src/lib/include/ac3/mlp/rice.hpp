#pragma once

#include <cstdint>

#include "ac3/core/bitreader.hpp"
#include "ac3/core/bitwriter.hpp"
#include "ac3/export.hpp"

// Golomb-Rice coding (Golomb, "Run-length encodings", IEEE Trans. Inform.
// Theory, 1966; Rice, "Some Practical Universal Noise Coding Techniques",
// JPL Publication 79-22, 1979) - a public-domain entropy code, independent
// of and predating MLP, for a geometrically/Laplacian-distributed
// non-negative source.
//
// UPDATE: Craven & Gerzon's "Lossless Coding for Audio Discs" (JAES Vol. 44
// No. 9, 1996) - the deepest of the three MLP papers read so far - makes
// clear this is NOT what MLP actually does. MLP's entropy stage is genuine
// table-driven Huffman coding: a block-adaptive selection among several
// prefix-code tables matched to Laplacian-shaped statistics, not a
// parameterised Rice code. This reconciles the patent's "17 tables" account
// (docs/concepts/truehd-mlp.md's "What the two patents actually describe")
// with the earlier AES papers' looser "Rice code... near-optimal" framing:
// the papers were describing the general problem class Rice coding also
// solves well, not literally claiming MLP implements textbook Rice coding.
//
// This module is kept as a well-defined, testable, near-optimal-for-Laplacian
// STAND-IN, not a claim to match block_data()'s real codeword format - that
// needs an actual Huffman table (or table family), not a Rice parameter. See
// docs/concepts/truehd-mlp.md's "What 'Lossless Coding for Audio Discs' adds"
// section. What's implemented here is provably correct on its own terms (round-trips any
// input, matches the well-known public algorithm), independent of whether
// it turns out to be MLP's literal wire format.

namespace ac3::mlp::rice {

// Interleaves signed integers onto the non-negative integers Rice coding
// operates on (0,-1,1,-2,2,... -> 0,1,2,3,4,...) - the standard mapping for
// applying a code built for magnitudes to a two-sided (here, Laplacian)
// residual distribution. Implemented via unsigned bit tricks so it is exact
// across the full int32_t range, including INT32_MIN, with no signed
// overflow.
[[nodiscard]] constexpr std::uint32_t zigzag_encode(std::int32_t value) {
    const auto v = static_cast<std::uint32_t>(value);
    const auto sign_mask = static_cast<std::uint32_t>(value >> 31);  // all-1s iff negative
    return (v << 1) ^ sign_mask;
}

[[nodiscard]] constexpr std::int32_t zigzag_decode(std::uint32_t value) {
    return static_cast<std::int32_t>((value >> 1) ^ (~(value & 1u) + 1u));
}

// Splits `value` into a quotient (value >> k), sent as that many 1-bits
// followed by a terminating 0-bit, and a k-bit remainder sent as plain
// binary - one of several equivalent unary-prefix conventions for the
// quotient; the choice here (1s-then-0) is an implementation decision, not
// yet confirmed against a real MLP bitstream. No escape/cap on the unary
// run: MLP's own worst case (peak-level white noise) falls back to plain
// PCM at the block level per the AES papers, rather than needing an
// in-band Rice escape.
AC3FORGE_EXPORT void encode(BitWriter& w, std::uint32_t value, int k);
[[nodiscard]] AC3FORGE_EXPORT std::uint32_t decode(BitReader& r, int k);

// The exact length encode() would produce, for cost estimation (e.g.
// searching for the best k for a block) without writing anything.
[[nodiscard]] constexpr int encoded_length(std::uint32_t value, int k) {
    return static_cast<int>(value >> k) + 1 + k;
}

}  // namespace ac3::mlp::rice
