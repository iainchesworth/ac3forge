#pragma once

#include <cstdint>
#include <span>
#include <utility>
#include <vector>

#include "ac3/export.hpp"

// The lossless matrix stage - US 7,193,538 B2's "Primitive Matrix Quantiser"
// (PMQ) cascade, independently confirmed by both AES papers' Fig. 4 "single
// affine transformation": an arbitrary lossless multichannel decorrelation
// matrix is decomposed into a sequence of steps, each of which modifies
// exactly ONE channel by adding a quantised linear combination of the
// OTHERS. "A trivial (though important) example would be the tendency of
// the matrix process to rotate a stereo mix from left/right to
// sum/difference" (JAES 2004, Sec. 4.1).
//
// This is a lifting-scheme construction: because a step only ever writes
// its own `target` and only ever reads OTHER channels, and decode replays
// every step in exactly reverse order, decode always recovers the exact
// pre-step values regardless of the coefficients chosen - the invertibility
// is structural, not something the coefficients have to be specially
// designed for (see build_restart_header-style test coverage in
// tests/test_mlp.cpp for the round-trip proof). What ISN'T yet resolved is
// how a real MLP encoder actually chooses/quantises/transmits these
// coefficients in block_data() - this module is the mathematically-provable
// primitive underneath that, not a claim to match Dolby's exact wire
// format. See docs/concepts/truehd-mlp.md.

namespace ac3::mlp::matrix {

// Round `value / 2^shift` to the nearest integer, ties away from zero.
// Implemented without relying on implementation-defined right-shift of a
// negative value: the sign is factored out before shifting.
[[nodiscard]] constexpr std::int64_t quantize(std::int64_t value, int shift) {
    if (shift <= 0) {
        return value;
    }
    const std::int64_t half = std::int64_t{1} << (shift - 1);
    return value >= 0 ? (value + half) >> shift : -((-value + half) >> shift);
}

// One PMQ step: samples[target] += quantize(sum(numerator_i * samples[source_i]), shift).
// `terms` must not reference `target` as a source - that would make the
// step depend on the very value it's about to overwrite, breaking the
// lifting-scheme invertibility argument above. Every term shares this
// step's one `shift`, per the patent's own account of a filter's
// coefficients "all... of the form m/16 or m/64" - one common denominator
// per step, not one per coefficient.
struct Step {
    int target = 0;
    int shift = 0;
    std::vector<std::pair<int, std::int32_t>> terms;  // {source channel, integer numerator}
};

// Applies `steps` in order (encode) or exactly reversed (decode) - the
// "cascade of affine transformations" both papers describe. `samples` holds
// one integer value per channel (a single sample instant across all
// channels the matrix spans); callers loop this per sample.
AC3FORGE_EXPORT void encode_cascade(std::span<const Step> steps, std::span<std::int64_t> samples);
AC3FORGE_EXPORT void decode_cascade(std::span<const Step> steps, std::span<std::int64_t> samples);

}  // namespace ac3::mlp::matrix
