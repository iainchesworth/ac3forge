#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <span>
#include <vector>

#include "ac3/core/bitalloc.hpp"
#include "golden/bitalloc_goldens.hpp"

TEST_CASE("bit allocation matches the independent Python reference bit-exactly", "[bitalloc]") {
    for (const auto& c : ac3::golden::kBitAllocCases) {
        CAPTURE(c.name);
        const std::span<const std::uint8_t> exps{c.exps.data(),
                                                 static_cast<std::size_t>(c.endmant)};
        std::vector<std::uint8_t> bap(static_cast<std::size_t>(c.endmant));
        const ac3::BitAllocCodes codes{.sdcycod = c.sdcycod,
                                       .fdcycod = c.fdcycod,
                                       .sgaincod = c.sgaincod,
                                       .dbpbcod = c.dbpbcod,
                                       .floorcod = c.floorcod,
                                       .fgaincod = c.fgaincod};
        // Single-channel cases, so the frame-wide §7.2.2.1.1 condition is
        // just this channel's offsets - matching what the Python reference
        // assumes when it generates these vectors.
        const ac3::BitAllocRegion region{
            .start = c.start,
            .coupling = c.coupling,
            .cplfleak = c.cplfleak,
            .cplsleak = c.cplsleak,
            .snr_all_zero = c.csnroffst == 0 && c.fsnroffst == 0,
            .delta = {.deltnseg = c.deltnseg,
                     .deltoffst = c.deltoffst,
                     .deltlen = c.deltlen,
                     .deltba = c.deltba}};
        ac3::compute_bit_allocation(exps, static_cast<ac3::SampleRate>(c.fscod), codes,
                                    c.csnroffst, c.fsnroffst, bap, region);
        // Only the allocated region is meaningful; bins below a coupling
        // channel's start are never touched by either implementation.
        for (int bin = c.start; bin < c.endmant; ++bin) {
            CAPTURE(bin);
            // Integer pseudocode: zero tolerance.
            REQUIRE(bap[static_cast<std::size_t>(bin)] == c.bap[static_cast<std::size_t>(bin)]);
        }
    }
}

TEST_CASE("snr offset composite formula", "[bitalloc]") {
    STATIC_CHECK(ac3::snr_offset(0, 0) == -960);
    STATIC_CHECK(ac3::snr_offset(15, 0) == 0);
    STATIC_CHECK(ac3::snr_offset(63, 15) == ((48 << 4) + 15) << 2);
}

TEST_CASE("choose_delta_segments finds a real vs. flat-model divergence", "[bitalloc]") {
    // exponent = -1 - log2(|c|) (see bitalloc.cpp's own derivation), so this
    // magnitude is the exact boundary the exponent alone would encode - zero
    // divergence from the flat model.
    constexpr int kExp = 10;
    constexpr int kEnd = 20;
    const std::vector<std::uint8_t> exps(kEnd, kExp);
    const double baseline = std::pow(2.0, -1.0 - kExp);
    std::vector<double> coeffs(kEnd, baseline);
    // Boost bins 5..9 by exactly one octave: +6 dB, one Table 5.17 step.
    for (int bin = 5; bin < 10; ++bin) {
        coeffs[static_cast<std::size_t>(bin)] = baseline * 2.0;
    }
    const auto segs = ac3::choose_delta_segments(coeffs, exps, 0);
    REQUIRE(segs.deltnseg == 1);
    CHECK(segs.deltoffst[0] == 5);  // bands 0..19 are 1:1 with bins here
    CHECK(segs.deltlen[0] == 5);
    CHECK(segs.deltba[0] == 4);  // +6 dB
}

TEST_CASE("choose_delta_segments is silent when content matches its exponents",
         "[bitalloc]") {
    constexpr int kExp = 8;
    constexpr int kEnd = 30;
    const std::vector<std::uint8_t> exps(kEnd, kExp);
    const double baseline = std::pow(2.0, -1.0 - kExp);
    const std::vector<double> coeffs(kEnd, baseline);
    const auto segs = ac3::choose_delta_segments(coeffs, exps, 0);
    CHECK(segs.deltnseg == 0);
}

TEST_CASE("monotonicity: more snr offset never allocates fewer bits", "[bitalloc]") {
    // The SNR search's binary search relies on this.
    std::vector<std::uint8_t> exps(253);
    for (std::size_t bin = 0; bin < exps.size(); ++bin) {
        exps[bin] = static_cast<std::uint8_t>((bin * 7 + 3) % 25);
    }
    std::vector<std::uint8_t> bap(253);
    const ac3::BitAllocCodes codes{};
    long long previous = -1;
    for (int composite = 0; composite <= 1023; composite += 51) {
        ac3::compute_bit_allocation(exps, ac3::SampleRate::k48000, codes, composite >> 4,
                                    composite & 15, bap,
                                    {.snr_all_zero = composite == 0});
        long long total = 0;
        for (const auto b : bap) {
            total += b;
        }
        CAPTURE(composite);
        CHECK(total >= previous);
        previous = total;
    }
}
