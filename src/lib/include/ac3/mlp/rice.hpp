#pragma once

#include <cstdint>

#include "ac3/core/bitreader.hpp"
#include "ac3/core/bitwriter.hpp"
#include "ac3/export.hpp"

// Golomb-Rice coding (Golomb, "Run-length encodings", IEEE Trans. Inform.
// Theory, 1966; Rice, "Some Practical Universal Noise Coding Techniques",
// JPL Publication 79-22, 1979) - a public-domain entropy code, independent
// of and predating MLP, for a geometrically/Laplacian-distributed
// non-negative source. Both AES papers (see docs/concepts/truehd-mlp.md)
// describe MLP's entropy stage in exactly these terms: "Audio signals often
// have a Laplacian distribution... The Rice code provides a simple and
// near-optimal way of encoding such a signal."
//
// This is NOT yet wired to block_data()'s actual bit-level codeword format -
// neither Dolby document specifies it, and the patent's account (17
// signal-adaptive Huffman tables, selected by peak level) hasn't been
// reconciled against this Rice-code framing yet; a per-block Rice parameter
// k chosen from peak level is a very plausible reading of "17 tables," but
// that's a hypothesis, not a confirmed equivalence - see
// docs/concepts/truehd-mlp.md's "What the AES papers add" section. What's
// implemented here is provably correct on its own terms (round-trips any
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
