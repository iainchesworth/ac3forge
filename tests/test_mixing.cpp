#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cmath>
#include <span>
#include <vector>

#include "ac3/core/tables.hpp"
#include "ac3/meta/mixing.hpp"

// ac3::meta::mixing (src/lib/src/meta/mixing.cpp) is the §7.8 downmix - what
// most listeners actually hear when a 5.1 (or narrower) programme folds down
// to a 2-speaker or 1-speaker system - plus the mix-metadata tables and LFE
// mix-level formula A/52 and E-AC-3's mixmdate carry. test_drc.cpp has two
// [mixing]-tagged tests that check stereo_downmix/mono_downmix's <=1 sum
// bound across every acmod and one exact-value case (3/2), plus
// mono_downmix_peak_dbfs's MDCT-overlap history behaviour - but nothing
// exercises mono_downmix's own per-acmod routing in the same exact-value
// detail stereo_downmix gets, the discrete-vs-spread surround distinction,
// the 1+1 dual-mono edge case, or the coefficient()/valid_surround_mix_level/
// lfe_mix_level_db tables at all (test_assignment.cpp/test_plan.cpp include
// mixing.hpp only for its enum types, never exercising the tables). These
// tests fill those gaps without repeating what test_drc.cpp already covers,
// using ac3::meta::level's own named constants for expected values rather
// than transcribed decimals, so a wrong table entry shows up as a mismatch
// against the spec's OWN constant, not a second hand-typed copy of the bug.

using ac3::meta::level::kMinus1_5dB;
using ac3::meta::level::kMinus3dB;
using ac3::meta::level::kMinus4_5dB;
using ac3::meta::level::kMinus6dB;
using ac3::meta::level::kPlus1_5dB;
using ac3::meta::level::kPlus3dB;
using ac3::meta::level::kSilent;
using ac3::meta::level::kUnity;

using Catch::Approx;

namespace {

double sum(const std::array<double, 5>& a) {
    double s = 0.0;
    for (const double v : a) {
        s += v;
    }
    return s;
}

}  // namespace

// --- stereo_downmix (Lo/Ro) -------------------------------------------------

TEST_CASE("stereo_downmix: 2/0 source passes straight through, unattenuated", "[mixing]") {
    // Already a stereo Lo/Ro signal - §7.8 has nothing to fold. Sum is
    // exactly 1 either way, so normalize() never touches it.
    const auto dm = ac3::meta::stereo_downmix(ac3::Acmod::k2_0, kMinus3dB, kMinus3dB);
    CHECK(dm.left == std::array<double, 5>{1.0, 0.0, 0.0, 0.0, 0.0});
    CHECK(dm.right == std::array<double, 5>{0.0, 1.0, 0.0, 0.0, 0.0});
}

TEST_CASE("stereo_downmix: 1+1 dual mono has no defined downmix and returns all zeros",
         "[mixing]") {
    const auto dm = ac3::meta::stereo_downmix(ac3::Acmod::kDualMono, kMinus3dB, kMinus3dB);
    CHECK(dm.left == std::array<double, 5>{});
    CHECK(dm.right == std::array<double, 5>{});
}

TEST_CASE("stereo_downmix: 3/0 with clev=unity sums to exactly 2 pre-normalization, "
         "so both surviving coefficients land on exactly 0.5",
         "[mixing]") {
    // left[0]=1 (L) and left[1]=clev=1 (C) before normalizing: equal
    // contributions, sum 2 > 1, so each is scaled by 1/2 - an exact result
    // that does not depend on transcribing any of normalize()'s division.
    const auto dm = ac3::meta::stereo_downmix(ac3::Acmod::k3_0, kUnity, kMinus3dB);
    CHECK(dm.left[0] == Approx(0.5));   // L
    CHECK(dm.left[1] == Approx(0.5));   // C
    CHECK(dm.left[2] == 0.0);           // R never reaches the left output
    CHECK(dm.right[1] == Approx(0.5));  // C
    CHECK(dm.right[2] == Approx(0.5));  // R
    CHECK(dm.right[0] == 0.0);          // L never reaches the right output
}

TEST_CASE("stereo_downmix: a discrete surround pair keeps its side, no cross-talk", "[mixing]") {
    // 2/2: Ls must contribute to Lo only, Rs to Ro only - unlike the single-
    // surround acmods below, there is no spreading across both fronts.
    const auto dm = ac3::meta::stereo_downmix(ac3::Acmod::k2_2, kMinus3dB, kMinus6dB);
    CHECK(dm.left[2] > 0.0);   // Ls -> Lo
    CHECK(dm.right[2] == 0.0);  // Ls does not leak into Ro
    CHECK(dm.right[3] > 0.0);  // Rs -> Ro
    CHECK(dm.left[3] == 0.0);  // Rs does not leak into Lo
}

TEST_CASE("stereo_downmix: a single coded surround spreads equally across both fronts",
         "[mixing]") {
    // 2/1: one surround channel, no surround loudspeaker pairing - §7.8
    // spreads it into both Lo and Ro identically (at slev - 3 dB each),
    // unlike 2/2's discrete pair above.
    const auto dm = ac3::meta::stereo_downmix(ac3::Acmod::k2_1, kMinus3dB, kUnity);
    CHECK(dm.left[2] > 0.0);
    CHECK(dm.left[2] == Approx(dm.right[2]));
}

TEST_CASE("stereo_downmix: 3/2 (full 5.1) combines centre and surround pair, "
         "each output stays within the <=1 sum bound",
         "[mixing]") {
    for (const auto clev : {kMinus3dB, kMinus4_5dB, kMinus6dB}) {
        for (const auto slev : {kMinus3dB, kMinus6dB, kSilent}) {
            CAPTURE(clev, slev);
            const auto dm = ac3::meta::stereo_downmix(ac3::Acmod::k3_2, clev, slev);
            double left_sum = 0.0, right_sum = 0.0;
            for (const double v : dm.left) {
                left_sum += v;
            }
            for (const double v : dm.right) {
                right_sum += v;
            }
            CHECK(left_sum <= 1.0 + 1e-9);
            CHECK(right_sum <= 1.0 + 1e-9);
            // L only ever reaches Lo, R only ever reaches Ro, in this layout.
            CHECK(dm.right[0] == 0.0);  // L (index 0)
            CHECK(dm.left[2] == 0.0);   // R (index 2)
            // Discrete surrounds keep their side here too.
            CHECK(dm.right[3] == 0.0);  // Ls
            CHECK(dm.left[4] == 0.0);   // Rs
        }
    }
}

// --- mono_downmix ------------------------------------------------------------

TEST_CASE("mono_downmix: 1/0 source passes straight through at unity, ignoring clev",
         "[mixing]") {
    // §7.8's 1/0 branch routes the centre straight through rather than
    // applying clev*+3dB - the +3 dB centre boost only applies when the
    // source actually has separate L/R to boost the centre relative to.
    const auto dm = ac3::meta::mono_downmix(ac3::Acmod::k1_0, kSilent, kMinus3dB);
    CHECK(dm[0] == 1.0);
    for (std::size_t i = 1; i < 5; ++i) {
        CHECK(dm[i] == 0.0);
    }
}

TEST_CASE("mono_downmix: 2/0 source folds L and R to equal weight, sum normalized to 1",
         "[mixing]") {
    // Both entries are the identical constant kMinus3dB before normalizing,
    // so - whatever that constant's value - dividing by their own sum always
    // lands each one on exactly 0.5: a/(a+a) = 1/2 for any a != 0.
    const auto dm = ac3::meta::mono_downmix(ac3::Acmod::k2_0, kMinus3dB, kMinus3dB);
    CHECK(dm[0] == Approx(0.5));
    CHECK(dm[1] == Approx(0.5));
    CHECK(sum(dm) == Approx(1.0));
}

TEST_CASE("mono_downmix: 2/1 with slev=unity gives three equal contributors, "
         "each normalized to exactly 1/3",
         "[mixing]") {
    // L, R and the single surround are all weighted kMinus3dB before
    // normalizing (slev=kUnity leaves the surround's slev*kMinus3dB term
    // equal to the other two) - three equal entries normalize to 1/3 each
    // regardless of what that shared constant actually is.
    const auto dm = ac3::meta::mono_downmix(ac3::Acmod::k2_1, kMinus3dB, kUnity);
    CHECK(dm[0] == Approx(1.0 / 3.0));
    CHECK(dm[1] == Approx(1.0 / 3.0));
    CHECK(dm[2] == Approx(1.0 / 3.0));
    CHECK(sum(dm) == Approx(1.0));
}

TEST_CASE("mono_downmix: 3/0 centre gets clev+3dB gain relative to L and R, preserving that "
         "ratio through normalization",
         "[mixing]") {
    const auto dm = ac3::meta::mono_downmix(ac3::Acmod::k3_0, kUnity, kMinus3dB);
    // Pre-normalization: L = R = kMinus3dB, C = kUnity*kPlus3dB = kPlus3dB.
    // L and R get the identical weight mono_downmix gives every 3-front-
    // channel source's outer pair, so they survive normalization equal to
    // each other; the ratio between either of them and C survives too, since
    // normalize() scales the whole vector uniformly.
    CHECK(dm[0] == dm[2]);
    CHECK(dm[1] / dm[0] == Approx(kPlus3dB / kMinus3dB));
    CHECK(sum(dm) == Approx(1.0));  // 2*kMinus3dB + kPlus3dB > 1, so this DID normalize
}

TEST_CASE("mono_downmix: 1+1 dual mono has no defined downmix and returns all zeros",
         "[mixing]") {
    const auto dm = ac3::meta::mono_downmix(ac3::Acmod::kDualMono, kMinus3dB, kMinus3dB);
    CHECK(dm == std::array<double, 5>{});
}

// --- mono_downmix_peak_dbfs --------------------------------------------------

TEST_CASE("mono_downmix_peak_dbfs: in-phase L/R sums to full scale, 0 dBFS", "[mixing][peak]") {
    const std::vector<float> left(64, 1.0f), right(64, 1.0f);
    const std::array<std::span<const float>, 2> chans{std::span<const float>{left},
                                                       std::span<const float>{right}};
    // 2/0's mono downmix is [0.5, 0.5] regardless of clev/slev (no centre or
    // surround exists to depend on them) - 0.5*1 + 0.5*1 = 1.0 = 0 dBFS.
    const double peak = ac3::meta::mono_downmix_peak_dbfs(
        std::span<const std::span<const float>>{chans}, ac3::Acmod::k2_0, kMinus3dB, kMinus3dB);
    CHECK(peak == Approx(0.0).margin(1e-6));
}

TEST_CASE("mono_downmix_peak_dbfs: out-of-phase L/R cancels in the mono sum", "[mixing][peak]") {
    const std::vector<float> left(64, 1.0f), right(64, -1.0f);
    const std::array<std::span<const float>, 2> chans{std::span<const float>{left},
                                                       std::span<const float>{right}};
    const double peak = ac3::meta::mono_downmix_peak_dbfs(
        std::span<const std::span<const float>>{chans}, ac3::Acmod::k2_0, kMinus3dB, kMinus3dB);
    // Exactly cancels (0.5*1 + 0.5*(-1) == 0 for every sample) - to_db's
    // documented silence sentinel, not -inf.
    CHECK(peak == -200.0);
}

TEST_CASE("mono_downmix_peak_dbfs: the two-history-block overload delegates to the "
         "empty-history one when there is no history",
         "[mixing][peak]") {
    const std::vector<float> left(32, 0.3f), right(32, -0.1f);
    const std::array<std::span<const float>, 2> chans{std::span<const float>{left},
                                                       std::span<const float>{right}};
    const double via_short = ac3::meta::mono_downmix_peak_dbfs(
        std::span<const std::span<const float>>{chans}, ac3::Acmod::k2_0, kMinus3dB, kMinus3dB);
    const double via_long = ac3::meta::mono_downmix_peak_dbfs(
        std::span<const std::array<double, 256>>{}, std::span<const std::span<const float>>{chans},
        ac3::Acmod::k2_0, kMinus3dB, kMinus3dB);
    CHECK(via_short == via_long);
}

TEST_CASE("mono_downmix_peak_dbfs: no channels pushed reads as silence, not -inf",
         "[mixing][peak]") {
    const double peak = ac3::meta::mono_downmix_peak_dbfs(
        std::span<const std::span<const float>>{}, ac3::Acmod::k2_0, kMinus3dB, kMinus3dB);
    CHECK(peak == -200.0);
}

// --- mix-level coefficient tables and LFE formula ---------------------------

TEST_CASE("coefficient(): CentreMixLevel matches Table 5.9's three legal codes", "[mixing]") {
    CHECK(ac3::meta::coefficient(ac3::meta::CentreMixLevel::kMinus3dB) == kMinus3dB);
    CHECK(ac3::meta::coefficient(ac3::meta::CentreMixLevel::kMinus4_5dB) == kMinus4_5dB);
    CHECK(ac3::meta::coefficient(ac3::meta::CentreMixLevel::kMinus6dB) == kMinus6dB);
}

TEST_CASE("coefficient(): SurroundMixLevel matches Table 5.10's three legal codes, "
         "including the genuine 'surrounds dropped' value",
         "[mixing]") {
    CHECK(ac3::meta::coefficient(ac3::meta::SurroundMixLevel::kMinus3dB) == kMinus3dB);
    CHECK(ac3::meta::coefficient(ac3::meta::SurroundMixLevel::kMinus6dB) == kMinus6dB);
    CHECK(ac3::meta::coefficient(ac3::meta::SurroundMixLevel::kSilent) == kSilent);
}

TEST_CASE("coefficient(): the 8-value MixLevel table (Tables D2.3/D2.5) is exact end to end",
         "[mixing]") {
    using ac3::meta::MixLevel;
    CHECK(ac3::meta::coefficient(MixLevel::kPlus3dB) == kPlus3dB);
    CHECK(ac3::meta::coefficient(MixLevel::kPlus1_5dB) == kPlus1_5dB);
    CHECK(ac3::meta::coefficient(MixLevel::kUnity) == kUnity);
    CHECK(ac3::meta::coefficient(MixLevel::kMinus1_5dB) == kMinus1_5dB);
    CHECK(ac3::meta::coefficient(MixLevel::kMinus3dB) == kMinus3dB);
    CHECK(ac3::meta::coefficient(MixLevel::kMinus4_5dB) == kMinus4_5dB);
    CHECK(ac3::meta::coefficient(MixLevel::kMinus6dB) == kMinus6dB);
    CHECK(ac3::meta::coefficient(MixLevel::kSilent) == kSilent);
}

TEST_CASE("valid_surround_mix_level: exactly the three reserved surround codes are refused",
         "[mixing]") {
    using ac3::meta::MixLevel;
    // Tables D2.4/D2.6 reserve '000'..'010' for surround use specifically -
    // these three values are legal MixLevel enumerators (used for
    // ltrtcmixlev/lorocmixlev) but must be refused for a surround field.
    CHECK_FALSE(ac3::meta::valid_surround_mix_level(MixLevel::kPlus3dB));
    CHECK_FALSE(ac3::meta::valid_surround_mix_level(MixLevel::kPlus1_5dB));
    CHECK_FALSE(ac3::meta::valid_surround_mix_level(MixLevel::kUnity));
    CHECK(ac3::meta::valid_surround_mix_level(MixLevel::kMinus1_5dB));
    CHECK(ac3::meta::valid_surround_mix_level(MixLevel::kMinus3dB));
    CHECK(ac3::meta::valid_surround_mix_level(MixLevel::kMinus4_5dB));
    CHECK(ac3::meta::valid_surround_mix_level(MixLevel::kMinus6dB));
    CHECK(ac3::meta::valid_surround_mix_level(MixLevel::kSilent));
}

TEST_CASE("lfe_mix_level_db: §E2.3.1.11's 10 - code formula across its full 0..31 range",
         "[mixing]") {
    CHECK(ac3::meta::lfe_mix_level_db(0) == 10.0);
    CHECK(ac3::meta::lfe_mix_level_db(10) == 0.0);
    CHECK(ac3::meta::lfe_mix_level_db(31) == -21.0);
    CHECK(ac3::meta::kLfeMixLevelIdeal == 0);
    CHECK(ac3::meta::lfe_mix_level_db(ac3::meta::kLfeMixLevelIdeal) == 10.0);
}
