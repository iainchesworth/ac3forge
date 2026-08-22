#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cmath>
#include <random>
#include <span>
#include <vector>

#include "ac3/core/mantissas.hpp"

namespace {

double to_unit(std::int32_t mantissa25) { return mantissa25 / 16777216.0; }

}  // namespace

TEST_CASE("quantize/dequantize round-trip error is within half a step", "[mantissas]") {
    std::mt19937 rng(0x073A);
    std::uniform_real_distribution<double> dist(-0.999, 0.999);
    for (int bap = 1; bap <= 15; ++bap) {
        const double step = bap <= 5
                                ? 2.0 / ac3::kSymmetricLevels[static_cast<std::size_t>(bap)]
                                : 1.0 / (1 << (ac3::kBapBits[static_cast<std::size_t>(bap)] - 1));
        // Asymmetric two's complement tops out at 1 - step (A/52 7.3.2:
        // the mantissa word spans (1.0 - 2^-(qntztab-1)) to -1.0); values
        // beyond that clamp by design and carry a larger error.
        const double max_representable = bap <= 5 ? 1.0 : 1.0 - step;
        for (int trial = 0; trial < 200; ++trial) {
            const double value = dist(rng);
            const auto mantissa = static_cast<std::int32_t>(std::lround(value * 16777216.0));
            const auto code = ac3::quantize_mantissa(mantissa, bap);
            const double reconstructed = ac3::dequantize_mantissa(code, bap);
            CAPTURE(bap, value, code);
            if (value <= max_representable + step / 2) {
                CHECK(std::abs(reconstructed - to_unit(mantissa)) <= step / 2 + 1e-9);
            } else {
                CHECK(reconstructed == max_representable);  // clamped to the top code
            }
        }
    }
}

TEST_CASE("symmetric quantizer maps zero to the middle code", "[mantissas]") {
    CHECK(ac3::quantize_mantissa(0, 1) == 1);   // 3-level: code 1 = 0
    CHECK(ac3::quantize_mantissa(0, 2) == 2);   // 5-level
    CHECK(ac3::quantize_mantissa(0, 3) == 3);   // 7-level
    CHECK(ac3::quantize_mantissa(0, 4) == 5);   // 11-level
    CHECK(ac3::quantize_mantissa(0, 5) == 7);   // 15-level
    for (int bap = 1; bap <= 5; ++bap) {
        CHECK(ac3::dequantize_mantissa(ac3::quantize_mantissa(0, bap), bap) == 0.0);
    }
}

TEST_CASE("grouping: codeword sits at first member, counts match", "[mantissas]") {
    // Hand case: five bap-1 mantissas (one full group + a padded partial),
    // one bap-3, two bap-4 (one pair). Codes: bap1 of 0 -> 1 (middle).
    ac3::MantissaBlockWriter writer;
    for (int i = 0; i < 5; ++i) {
        writer.add(0, 1);
    }
    writer.add(0, 3);
    writer.add(0, 4);
    writer.add(0, 4);
    writer.finish_block();

    const auto& tokens = writer.tokens();
    REQUIRE(tokens.size() == 4);
    CHECK(tokens[0].bits == 5);   // bap-1 group of codes 1,1,1 -> 9+3+1 = 13
    CHECK(tokens[0].value == 13);
    CHECK(tokens[1].bits == 5);   // partial group: codes 1,1 + dummy 0 -> 12
    CHECK(tokens[1].value == 12);
    CHECK(tokens[2].bits == 3);   // direct bap-3 code 3
    CHECK(tokens[2].value == 3);
    CHECK(tokens[3].bits == 7);   // bap-4 pair codes 5,5 -> 55*... 11*5+5 = 60
    CHECK(tokens[3].value == 60);
    CHECK(writer.bit_count() == 5 + 5 + 3 + 7);
}

TEST_CASE("bit counter agrees with the writer for random allocations", "[mantissas]") {
    std::mt19937 rng(0x0B77);
    std::uniform_int_distribution<int> bap_dist(0, 15);
    std::uniform_int_distribution<std::int32_t> mant_dist(-16777216, 16777215);

    for (int trial = 0; trial < 30; ++trial) {
        std::array<std::vector<std::uint8_t>, 2> baps;
        ac3::MantissaBlockWriter writer;
        for (auto& channel : baps) {
            channel.resize(253);
            for (auto& bap : channel) {
                bap = static_cast<std::uint8_t>(bap_dist(rng));
            }
        }
        for (const auto& channel : baps) {
            for (const auto bap : channel) {
                writer.add(mant_dist(rng), bap);
            }
        }
        writer.finish_block();

        const std::array<std::span<const std::uint8_t>, 2> views = {baps[0], baps[1]};
        std::size_t token_bits = 0;
        for (const auto& token : writer.tokens()) {
            token_bits += token.bits;
        }
        CAPTURE(trial);
        CHECK(writer.bit_count() == ac3::mantissa_bits_per_block(views));
        CHECK(token_bits == writer.bit_count());
    }
}
