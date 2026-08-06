#include <catch2/catch_test_macros.hpp>

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
        ac3::compute_bit_allocation(exps, static_cast<ac3::SampleRate>(c.fscod), codes,
                                    c.csnroffst, c.fsnroffst, bap);
        for (int bin = 0; bin < c.endmant; ++bin) {
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
                                    composite & 15, bap);
        long long total = 0;
        for (const auto b : bap) {
            total += b;
        }
        CAPTURE(composite);
        CHECK(total >= previous);
        previous = total;
    }
}
