#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <numbers>
#include <random>
#include <span>

#include "ac3/core/mdct.hpp"
#include "golden/mdct_goldens.hpp"

// Phase 4 of the performance-observability programme: the opt-in §7.9.4
// fast N/4-FFT MDCT (mdct.cpp's mdct_forward_fast_core, reached via
// mdct512_forward's `fast` parameter). This file is the correctness half -
// the fast path must agree with the existing direct-form path (already
// validated against numpy goldens in test_mdct.cpp) to within a tight
// numerical tolerance, on both synthetic and real-audio-shaped input, before
// anything is allowed to default it on.
//
// Only the long transform (alpha = 0) has an accelerated path today.
// mdct256_forward_first/second (the block-switched short transforms) do
// not - see mdct.hpp's own comment on why an earlier attempt to reuse the
// same fold for alpha = -1 turned out to be a math error, not just
// unimplemented - so their own test below checks that `fast=true` and
// `fast=false` are IDENTICAL, not just close, since they run the same code.

namespace {

// Relative error, normalized against the SPECTRUM's own peak magnitude
// rather than each bin's individual value. A per-bin denominator blows up on
// exactly the input this test cares about: a real (tone) signal concentrates
// almost all its energy into a handful of bins, leaving the rest at genuine
// spectral-leakage magnitudes (1e-8..1e-11) where a few ULPs of ordinary
// floating-point rounding - present between ANY two algorithms that do not
// perform IDENTICAL operations in IDENTICAL order, fast-vs-direct included -
// reads as a huge per-bin relative error despite being scientifically
// meaningless at that magnitude. Peak-normalizing is the standard way to
// compare two spectra and avoids that trap while staying scale-invariant.
double max_rel_error(std::span<const double> fast, std::span<const double> direct) {
    double peak = 0.0;
    for (const double v : direct) {
        peak = std::max(peak, std::abs(v));
    }
    const double denom = std::max(peak, 1e-12);
    double worst_abs = 0.0;
    for (std::size_t i = 0; i < direct.size(); ++i) {
        worst_abs = std::max(worst_abs, std::abs(fast[i] - direct[i]));
    }
    return worst_abs / denom;
}

constexpr double kFastTolerance = 1e-10;  // matches mdct.hpp's own documented bound

std::array<double, 256> forward512(const std::array<double, 512>& input, bool fast) {
    std::array<double, 512> windowed{};
    std::array<double, 256> coeffs{};
    ac3::apply_analysis_window(input, windowed);
    ac3::mdct512_forward(windowed, coeffs, fast);
    return coeffs;
}

struct ShortCoeffs {
    std::array<double, 128> first;
    std::array<double, 128> second;
};

ShortCoeffs forward256_pair(const std::array<double, 512>& input, bool fast) {
    std::array<double, 512> windowed{};
    ac3::apply_analysis_window(input, windowed);
    const std::span<const double, 512> full(windowed);
    ShortCoeffs out{};
    ac3::mdct256_forward_first(full.first<256>(), out.first, fast);
    ac3::mdct256_forward_second(full.last<256>(), out.second, fast);
    return out;
}

std::array<double, 512> random_block(std::uint32_t seed) {
    std::mt19937 rng(seed);
    std::uniform_real_distribution<double> dist(-1.0, 1.0);
    std::array<double, 512> block{};
    for (auto& s : block) {
        s = dist(rng);
    }
    return block;
}

// Three real-audio-shaped blocks (not silence, not a single frequency): a
// couple of tones summed together, at different phases per block so the
// three cover different spectral shapes the way consecutive real frames
// would.
std::array<double, 512> tone_block(int block_offset) {
    std::array<double, 512> block{};
    for (int n = 0; n < 512; ++n) {
        const double t = static_cast<double>(block_offset * 256 + n) / 48000.0;
        block[static_cast<std::size_t>(n)] =
            0.3 * std::sin(2.0 * std::numbers::pi * 440.0 * t) +
            0.15 * std::sin(2.0 * std::numbers::pi * 2500.0 * t);
    }
    return block;
}

}  // namespace

TEST_CASE("fast mdct512_forward agrees with the direct form on numpy goldens", "[mdct][fast]") {
    const auto check = [](const std::array<double, 512>& input,
                          std::span<const double> expected) {
        const auto direct = forward512(input, false);
        const auto fast = forward512(input, true);
        CHECK(max_rel_error(direct, expected) < 1e-9);  // sanity: direct still matches goldens
        CHECK(max_rel_error(fast, direct) < kFastTolerance);
    };
    check(ac3::golden::kGoldenImpulse0Input, ac3::golden::kGoldenImpulse0Coeffs);
    check(ac3::golden::kGoldenImpulse100Input, ac3::golden::kGoldenImpulse100Coeffs);
    check(ac3::golden::kGoldenDcInput, ac3::golden::kGoldenDcCoeffs);
    check(ac3::golden::kGoldenSineInput, ac3::golden::kGoldenSineCoeffs);
    check(ac3::golden::kGoldenRandomInput, ac3::golden::kGoldenRandomCoeffs);
}

TEST_CASE("fast mdct512_forward agrees with the direct form on random data", "[mdct][fast]") {
    for (std::uint32_t seed = 0; seed < 8; ++seed) {
        CAPTURE(seed);
        const auto block = random_block(0x5eed0000U + seed);
        const auto direct = forward512(block, false);
        const auto fast = forward512(block, true);
        CHECK(max_rel_error(fast, direct) < kFastTolerance);
    }
}

TEST_CASE("fast mdct512_forward agrees with the direct form on real audio", "[mdct][fast]") {
    // 3+ frames of real (tone) audio, never silence or a single sample - see
    // this project's own "silence gives false passes" lesson, which applies
    // just as much to a numerical-agreement test as to a codec-correctness
    // one: a degenerate input can trivially satisfy a tolerance check
    // without exercising the algorithm's real behaviour.
    for (int block = 0; block < 6; ++block) {
        CAPTURE(block);
        const auto input = tone_block(block);
        const auto direct = forward512(input, false);
        const auto fast = forward512(input, true);
        CHECK(max_rel_error(fast, direct) < kFastTolerance);
    }
}

TEST_CASE("fast mdct256_forward_first/second agree with their direct forms",
         "[mdct][fast]") {
    // Each short transform's fold is verified against ITS OWN direct-form
    // table, separately - the lesson behind this file's whole existence: an
    // earlier attempt reused the alpha = 0 fold for alpha = -1 on the
    // reasoning that "phi_k(-1) = 0 makes them the same formula", and a
    // numerical check exactly like this one is what caught the ~1.85
    // relative error before it reached a bitstream. alpha = -1's fold is
    // the DCT-IV of the antisymmetric half-fold; alpha = +1's reaches the
    // same core through the DST-IV reversal identity - two different
    // derivations, each of which must independently survive this bound.
    const auto check = [](const std::array<double, 512>& input) {
        const auto direct = forward256_pair(input, false);
        const auto fast = forward256_pair(input, true);
        CHECK(max_rel_error(fast.first, direct.first) < kFastTolerance);
        CHECK(max_rel_error(fast.second, direct.second) < kFastTolerance);
    };
    check(ac3::golden::kGoldenSineInput);
    check(ac3::golden::kGoldenRandomInput);
    for (std::uint32_t seed = 0; seed < 8; ++seed) {
        CAPTURE(seed);
        check(random_block(0x5ec00000U + seed));
    }
    for (int block = 0; block < 6; ++block) {
        CAPTURE(block);
        check(tone_block(block));
    }
}
