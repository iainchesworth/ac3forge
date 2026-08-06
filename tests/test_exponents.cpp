#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstdint>
#include <random>
#include <vector>

#include "ac3/core/exponents.hpp"
#include "ac3/encoder/silent_frame.hpp"

namespace {

std::vector<std::uint8_t> decode_all(const ac3::EncodedExponents& encoded,
                                     ac3::ExpStrategy strategy, int endmant) {
    std::vector<std::uint8_t> out(static_cast<std::size_t>(endmant));
    ac3::decode_exponents(encoded.absolute, encoded.groups, strategy, out);
    return out;
}

}  // namespace

TEST_CASE("fixed-point conversion and exponent extraction", "[exponents]") {
    using ac3::exponent_from_fixed;
    using ac3::to_fixed25;

    // |c| in [0.5, 1) -> exponent 0; each halving adds one.
    CHECK(exponent_from_fixed(to_fixed25(0.5)) == 0);
    CHECK(exponent_from_fixed(to_fixed25(-0.75)) == 0);
    CHECK(exponent_from_fixed(to_fixed25(0.49)) == 1);
    CHECK(exponent_from_fixed(to_fixed25(0.25)) == 1);
    CHECK(exponent_from_fixed(to_fixed25(0.2499)) == 2);
    // Saturation and the quiet end.
    CHECK(exponent_from_fixed(to_fixed25(1.0)) == 0);
    CHECK(exponent_from_fixed(to_fixed25(-1.0)) == 0);
    CHECK(exponent_from_fixed(to_fixed25(5.9e-8)) == 23);  // ~2^-24
    CHECK(exponent_from_fixed(to_fixed25(1e-9)) == ac3::kMaxExponent);  // rounds to 0
    CHECK(exponent_from_fixed(0) == ac3::kMaxExponent);
    CHECK(to_fixed25(2.0) == 16777215);
    CHECK(to_fixed25(-2.0) == -16777216);
}

TEST_CASE("group-count formulas match the spec examples", "[exponents]") {
    using ac3::exponent_group_count;
    using enum ac3::ExpStrategy;
    // endmant = 73 (chbwcod 0): 24 / 12 / 6 groups (A/52 7.1.3).
    STATIC_CHECK(exponent_group_count(kD15, 73) == 24);
    STATIC_CHECK(exponent_group_count(kD25, 73) == 12);
    STATIC_CHECK(exponent_group_count(kD45, 73) == 6);
    // The LFE channel: 7 mantissas, D15, nlfegrps = 2 (5.4.3.29).
    STATIC_CHECK(exponent_group_count(kD15, 7) == 2);
    // endmant = 253 (chbwcod 60, full bandwidth): 84 D15 groups.
    STATIC_CHECK(exponent_group_count(kD15, 253) == 84);
}

TEST_CASE("silent-frame exponent fields fall out of the general encoder", "[exponents]") {
    // The hand-built Milestone-2 silent frame drives all 73 bins to exponent
    // 24 from an absolute of 15; the independent bitstream-parse audit
    // validated those fields externally, so they serve as a golden here.
    std::array<std::uint8_t, 73> raw{};
    raw.fill(ac3::kMaxExponent);

    const auto encoded = ac3::encode_exponents(raw, ac3::ExpStrategy::kD15);
    CHECK(encoded.absolute == 15);
    REQUIRE(encoded.groups.size() == ac3::detail::kSilentExpGroups.size());
    for (std::size_t i = 0; i < encoded.groups.size(); ++i) {
        CAPTURE(i);
        CHECK(encoded.groups[i] == ac3::detail::kSilentExpGroups[i]);
    }

    // Decoder mirror: the ramp 15,17,19,21,23,24,24,...
    const auto decoded = decode_all(encoded, ac3::ExpStrategy::kD15, 73);
    CHECK(decoded[0] == 15);
    CHECK(decoded[1] == 17);
    CHECK(decoded[4] == 23);
    CHECK(decoded[5] == 24);
    for (std::size_t bin = 5; bin < decoded.size(); ++bin) {
        CHECK(decoded[bin] == 24);
    }
}

TEST_CASE("hand-computed D25 case incl. absolute-exponent lowering", "[exponents]") {
    // raw = {10, 5,5, 9,9, 3,3}, endmant 7 (a legal size: the LFE count).
    // Pair minima: 5, 9, 3 -> pre = [10, 5, 9, 3]. Forward slew caps the
    // rise to 9 at 5+2=7; backward slew then lowers 7 -> 3+2=5 and must
    // LOWER the absolute exponent 10 -> 5+2=7 so every step fits +-2:
    // final pre = [7, 5, 5, 3], diffs -2, 0, -2 -> mapped 0, 2, 0 -> 10.
    const std::array<std::uint8_t, 7> raw = {10, 5, 5, 9, 9, 3, 3};
    const auto encoded = ac3::encode_exponents(raw, ac3::ExpStrategy::kD25);
    CHECK(encoded.absolute == 7);
    REQUIRE(encoded.groups.size() == 1);
    CHECK(encoded.groups[0] == 10);

    const auto decoded = decode_all(encoded, ac3::ExpStrategy::kD25, 7);
    CHECK(decoded == std::vector<std::uint8_t>{7, 5, 5, 5, 5, 3, 3});
}

TEST_CASE("encode/decode properties over random exponent sets", "[exponents]") {
    std::mt19937 rng(0x0715);
    std::uniform_int_distribution<int> exp_dist(0, ac3::kMaxExponent);

    // Legal AC-3 mantissa counts only: fbw channels 73..253 step 3 (chbwcod
    // 0..60), coupled channels 37 + 12*cplbegf, and the 7-mantissa LFE. All
    // satisfy (endmant - 1) % 3 == 0, the coverage contract of 7.1.3.
    std::vector<int> legal_endmant{7};
    for (int cbw = 0; cbw <= 60; ++cbw) {
        legal_endmant.push_back(((cbw + 12) * 3) + 37);
    }
    for (int cplbegf = 0; cplbegf <= 14; ++cplbegf) {
        legal_endmant.push_back(37 + 12 * cplbegf);
    }
    std::uniform_int_distribution<std::size_t> endmant_pick(0, legal_endmant.size() - 1);

    for (const auto strategy :
         {ac3::ExpStrategy::kD15, ac3::ExpStrategy::kD25, ac3::ExpStrategy::kD45}) {
        for (int trial = 0; trial < 40; ++trial) {
            const int endmant = legal_endmant[endmant_pick(rng)];
            std::vector<std::uint8_t> raw(static_cast<std::size_t>(endmant));
            for (auto& e : raw) {
                e = static_cast<std::uint8_t>(exp_dist(rng));
            }
            CAPTURE(static_cast<int>(strategy), endmant, trial);

            const auto encoded = ac3::encode_exponents(raw, strategy);
            CHECK(encoded.absolute <= ac3::kMaxAbsoluteExponent);
            CHECK(static_cast<int>(encoded.groups.size()) ==
                  ac3::exponent_group_count(strategy, endmant));
            for (const auto g : encoded.groups) {
                CHECK(g <= 124);  // 25*4 + 5*4 + 4: every mapped value <= 4
            }

            const auto decoded = decode_all(encoded, strategy, endmant);

            // Safety: the decoder-mirror exponent never exceeds the raw one
            // (larger exponent would make the true mantissa unrepresentable).
            for (int bin = 0; bin < endmant; ++bin) {
                CAPTURE(bin);
                CHECK(decoded[static_cast<std::size_t>(bin)] <= raw[static_cast<std::size_t>(bin)]);
                CHECK(decoded[static_cast<std::size_t>(bin)] <= ac3::kMaxExponent);
            }

            // Pairs/quads share one exponent (A/52 7.1.3 step 4).
            const int group_size = ac3::exponent_group_size(strategy);
            for (int bin = 1; bin < endmant; ++bin) {
                if ((bin - 1) % group_size != 0) {
                    CHECK(decoded[static_cast<std::size_t>(bin)] ==
                          decoded[static_cast<std::size_t>(bin - 1)]);
                }
            }

            // Encoding the decoded set again is a fixpoint: identical fields.
            const auto re_encoded = ac3::encode_exponents(decoded, strategy);
            CHECK(re_encoded.absolute == encoded.absolute);
            CHECK(re_encoded.groups == encoded.groups);
        }
    }
}
