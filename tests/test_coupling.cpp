#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cmath>
#include <random>
#include <vector>

#include "ac3/core/exponents.hpp"
#include "ac3/encoder/coupling.hpp"

using ac3::coupling::choose_master;
using ac3::coupling::decode_coordinate;
using ac3::coupling::quantize_coordinate;

TEST_CASE("coupling sub-band geometry matches A/52 Table 7.24", "[coupling]") {
    // Sub-band 0 spans coefficients 37..48, sub-band 17 spans 241..252.
    STATIC_CHECK(ac3::coupling::start_mant(0) == 37);
    STATIC_CHECK(ac3::coupling::end_mant(15) == 253);
    STATIC_CHECK(ac3::coupling::start_mant(6) == 109);
    // cplendf is read by adding 3, so cplendf=12 ends at sub-band 15.
    STATIC_CHECK(ac3::coupling::end_mant(12) == 217);
    STATIC_CHECK(ac3::coupling::sub_band_count(6, 12) == 9);
    STATIC_CHECK(ac3::coupling::sub_band_count(0, 15) == 18);
}

TEST_CASE("decode_coordinate follows the spec's two mantissa forms", "[coupling]") {
    // exp < 15: value = (mant + 16) / 32, then >> exp.
    CHECK(decode_coordinate({.exp = 0, .mant = 16 - 16}, 0) == 0.5);
    CHECK(decode_coordinate({.exp = 0, .mant = 15}, 0) == 31.0 / 32.0);
    CHECK(decode_coordinate({.exp = 1, .mant = 0}, 0) == 0.25);
    // exp == 15: value = mant / 16, then >> 15.
    CHECK(decode_coordinate({.exp = 15, .mant = 8}, 0) == std::ldexp(0.5, -15));
    CHECK(decode_coordinate({.exp = 15, .mant = 0}, 0) == 0.0);
    // The master adds 3 exponent steps per unit.
    CHECK(decode_coordinate({.exp = 0, .mant = 0}, 1) == std::ldexp(0.5, -3));
    CHECK(decode_coordinate({.exp = 0, .mant = 0}, 3) == std::ldexp(0.5, -9));
}

TEST_CASE("quantized coordinates round-trip within a quantizer step", "[coupling]") {
    // Sweep the useful dynamic range. With the implicit leading one the
    // mantissa has 5 bits of resolution, so relative error stays under ~3%.
    for (int db = 0; db >= -84; --db) {
        const double value = std::pow(10.0, db / 20.0);
        const std::array<double, 1> single = {value};
        const int master = choose_master(single);
        const auto coordinate = quantize_coordinate(value, master);
        const double back = decode_coordinate(coordinate, master);
        CAPTURE(db, value, master, coordinate.exp, coordinate.mant, back);
        REQUIRE(coordinate.exp <= 15);
        REQUIRE(coordinate.mant <= 15);
        CHECK(std::abs(back - value) <= value * 0.05 + 1e-9);
    }
}

TEST_CASE("a shared master still covers a wide spread of bands", "[coupling]") {
    // Realistic case: one channel dominates a band and is near-absent in
    // another. One master must serve every band of that channel.
    const std::vector<double> values = {0.98, 0.5, 0.2, 0.05, 0.01, 0.002, 0.0004, 0.0};
    const int master = choose_master(values);
    CAPTURE(master);
    for (const double value : values) {
        const auto coordinate = quantize_coordinate(value, master);
        const double back = decode_coordinate(coordinate, master);
        CAPTURE(value, coordinate.exp, coordinate.mant, back);
        REQUIRE(coordinate.exp <= 15);
        REQUIRE(coordinate.mant <= 15);
        // Loud bands must stay accurate; the very quietest may bottom out,
        // which is inaudible under the louder bands around it.
        if (value > 1e-3) {
            CHECK(std::abs(back - value) <= value * 0.05 + 1e-9);
        } else {
            CHECK(back <= value * 1.5 + 1e-4);
        }
    }
}

TEST_CASE("coupling exponent set round-trips through the normative decode", "[coupling]") {
    // The coupling channel's absolute exponent is an even-valued reference
    // that is NOT itself a coefficient exponent, so encode/decode must agree
    // on the off-by-one or every coupled bin lands on the wrong scale.
    for (const int nsubbands : {1, 4, 9, 18}) {
        const int count = nsubbands * ac3::coupling::kBinsPerSubBand;
        std::vector<std::uint8_t> raw(static_cast<std::size_t>(count));
        for (int i = 0; i < count; ++i) {
            raw[static_cast<std::size_t>(i)] =
                static_cast<std::uint8_t>(3 + (i * 7) % 20);
        }
        const auto encoded = ac3::encode_coupling_exponents(raw, ac3::ExpStrategy::kD15);
        CAPTURE(nsubbands, count, encoded.cplabsexp, encoded.groups.size());
        REQUIRE(encoded.cplabsexp <= 12);  // absexp is even and at most 24
        REQUIRE(static_cast<int>(encoded.groups.size()) == count / 3);
        for (const auto group : encoded.groups) {
            REQUIRE(group <= 124);
        }

        std::vector<std::uint8_t> decoded(static_cast<std::size_t>(count));
        ac3::decode_coupling_exponents(encoded.cplabsexp, encoded.groups,
                                       ac3::ExpStrategy::kD15, decoded);
        for (int i = 0; i < count; ++i) {
            CAPTURE(i);
            // Same safety invariant as fbw exponents: never larger than the
            // raw value, or the true mantissa becomes unrepresentable.
            REQUIRE(decoded[static_cast<std::size_t>(i)] <=
                    raw[static_cast<std::size_t>(i)]);
            REQUIRE(decoded[static_cast<std::size_t>(i)] <= ac3::kMaxExponent);
        }
    }
}

TEST_CASE("the mean coupling divisor keeps coordinates representable",
          "[coupling]") {
    // §7.4.1's coupling channel is the mean of the coupled channels, so the
    // transmitted coordinate is ratio * nfchans / 8 with ratio =
    // sqrt(E_ch / E_sum). That is a level-free number - which is what lets
    // one coordinate serve two blocks - but it is NOT bounded: partial
    // cancellation between the channels shrinks E_sum without shrinking
    // E_ch, and the field stops at 0.96875. This pins down where that
    // actually bites, so the limit is a measured fact rather than a hope.
    //
    // Scaling the coupling channel up per band would dodge the ceiling, and
    // costs far more than it saves; see the encoder's own note and the
    // "coupling must not cost more bits" test.
    std::mt19937 rng(0x0C0F);
    std::uniform_real_distribution<double> dist(-1.0, 1.0);
    constexpr int kBins = ac3::coupling::kBinsPerSubBand;
    constexpr double kCeiling = 31.0 / 32.0;

    for (int trial = 0; trial < 500; ++trial) {
        // Two channels whose correlation runs from anti-phase through
        // independent to identical.
        const double mix = dist(rng);
        double energy_a = 0.0;
        double energy_sum = 0.0;
        for (int i = 0; i < kBins; ++i) {
            const double a = dist(rng);
            const double b = mix * a + 0.02 * dist(rng);
            energy_a += a * a;
            energy_sum += (a + b) * (a + b);
        }
        if (energy_sum <= 0.0) {
            continue;
        }
        const double ratio = std::sqrt(energy_a / energy_sum);
        const double coordinate = ratio * 2.0 / 8.0;  // stereo: nfchans == 2
        CAPTURE(trial, mix, ratio, coordinate);

        // Anything short of the channels cancelling stays inside the field:
        // stereo has room up to ratio 3.875, which is E_sum nearly 12 dB
        // below E_ch.
        if (mix > -0.5) {
            REQUIRE(coordinate < kCeiling);
        }

        const std::array<double, 1> single = {coordinate};
        const int master = choose_master(single);
        const auto encoded = quantize_coordinate(coordinate, master);
        const double back = decode_coordinate(encoded, master);
        if (coordinate < kCeiling) {
            CHECK(std::abs(back - coordinate) <= coordinate * 0.05 + 1e-9);
        } else {
            // Beyond it the quantizer clamps rather than wrapping, so the
            // band comes out quiet instead of arriving at the wrong level.
            CHECK(back == kCeiling);
        }
    }
}

TEST_CASE("quantization clamps gracefully rather than wrapping", "[coupling]") {
    // Out-of-range inputs should not be reachable from the encoder, but the
    // quantizer must still produce legal 4-bit fields if handed one.
    for (const double value : {1.0, 1.5, 4.0}) {
        const auto coordinate = quantize_coordinate(value, 0);
        CAPTURE(value, coordinate.exp, coordinate.mant);
        CHECK(coordinate.exp <= 15);
        CHECK(coordinate.mant <= 15);
        CHECK(decode_coordinate(coordinate, 0) <= 1.0);
    }
    // Zero and negatives collapse to silence, not to a huge coordinate.
    CHECK(decode_coordinate(quantize_coordinate(0.0, 0), 0) == 0.0);
    CHECK(decode_coordinate(quantize_coordinate(-1.0, 0), 0) == 0.0);
}
