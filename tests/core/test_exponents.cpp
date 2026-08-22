#include <catch2/catch_test_macros.hpp>

#include <algorithm>
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

TEST_CASE("decoded exponents stay in 0..24 for adversarial profiles", "[exponents]") {
    // §7.2.2.2 requires every decoded exponent to land in 0..24, and this
    // project's own decoder rejects a frame that breaks it (decoder.cpp's
    // kInvalidStream guard) - the mantissa reconstruction shifts by the
    // exponent, so an out-of-range value is undefined behaviour, not merely
    // wrong audio. The random sweep below samples uniformly and so never
    // builds the shape that actually stresses the differential chain: a
    // steep monotone rise, where clamping each delta to +-2 means the
    // reconstruction cannot track the profile exactly and drifts. Wide (D45)
    // groups widen that gap further, because the group-minimum preprocessing
    // discards three of every four bins. These profiles pin that the drift
    // can only ever go DOWNWARD, for every strategy and every legal endmant.
    std::vector<int> legal_endmant{7};
    for (int cbw = 0; cbw <= 60; ++cbw) {
        legal_endmant.push_back(((cbw + 12) * 3) + 37);
    }
    for (int cplbegf = 0; cplbegf <= 14; ++cplbegf) {
        legal_endmant.push_back(37 + 12 * cplbegf);
    }

    for (const auto strategy :
         {ac3::ExpStrategy::kD15, ac3::ExpStrategy::kD25, ac3::ExpStrategy::kD45}) {
        for (const int endmant : legal_endmant) {
            const auto size = static_cast<std::size_t>(endmant);

            // Build the stress shapes for this size.
            std::vector<std::vector<std::uint8_t>> profiles;
            const auto add = [&](std::vector<std::uint8_t> p) { profiles.push_back(std::move(p)); };

            add(std::vector<std::uint8_t>(size, ac3::kMaxExponent));  // digital silence
            add(std::vector<std::uint8_t>(size, 0));                  // full scale everywhere

            for (const int slope : {1, 2, 3, 6, 12, 24}) {
                for (const int from : {0, endmant / 2, endmant - 12, endmant - 4, endmant - 1}) {
                    if (from < 0) {
                        continue;
                    }
                    // A steep rise that only starts partway up the band - the
                    // "quiet top end" shape the slew limiter has to absorb.
                    std::vector<std::uint8_t> rise(size);
                    for (int bin = 0; bin < endmant; ++bin) {
                        const int v = bin <= from ? 0 : (bin - from) * slope;
                        rise[static_cast<std::size_t>(bin)] =
                            static_cast<std::uint8_t>(std::min(v, ac3::kMaxExponent));
                    }
                    add(rise);

                    // And its mirror: a steep fall, which the backward pass
                    // has to absorb by lowering the absolute exponent.
                    std::vector<std::uint8_t> fall(size);
                    for (int bin = 0; bin < endmant; ++bin) {
                        const int v = bin <= from ? ac3::kMaxExponent
                                                  : ac3::kMaxExponent - (bin - from) * slope;
                        fall[static_cast<std::size_t>(bin)] =
                            static_cast<std::uint8_t>(std::max(v, 0));
                    }
                    add(fall);
                }
            }

            // Defence in depth: raw exponents ABOVE the documented 0..24
            // precondition must still not be able to produce an illegal
            // stream. The group-minimum preprocessing seeds each position at
            // kMaxExponent, so out-of-contract input is clamped rather than
            // carried into the differential chain.
            add(std::vector<std::uint8_t>(size, 255));
            {
                std::vector<std::uint8_t> past(size);
                for (int bin = 0; bin < endmant; ++bin) {
                    past[static_cast<std::size_t>(bin)] =
                        static_cast<std::uint8_t>(std::min(10 + 3 * bin, 255));
                }
                add(past);
            }

            for (std::size_t p = 0; p < profiles.size(); ++p) {
                CAPTURE(static_cast<int>(strategy), endmant, p);
                const auto& raw = profiles[p];

                const auto encoded = ac3::encode_exponents(raw, strategy);
                CHECK(encoded.absolute <= ac3::kMaxAbsoluteExponent);
                REQUIRE(static_cast<int>(encoded.groups.size()) ==
                        ac3::exponent_group_count(strategy, endmant));
                // A grouped value above 124 is not a legal triple of mapped
                // values (§7.10.2 error condition 17) - it means some
                // differential escaped the +-2 range Table 7.1 can carry.
                for (const auto g : encoded.groups) {
                    CHECK(g <= 124);
                }

                const auto decoded = decode_all(encoded, strategy, endmant);
                for (int bin = 0; bin < endmant; ++bin) {
                    CAPTURE(bin);
                    CHECK(decoded[static_cast<std::size_t>(bin)] <= ac3::kMaxExponent);
                }
            }
        }
    }
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
