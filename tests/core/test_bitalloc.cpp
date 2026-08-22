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

TEST_CASE("choose_delta_segments targets bands relative to the channel's own "
         "start, not band 0",
         "[bitalloc]") {
    // Coupling channel starting at bin 37 - Table 7.13 maps that to band 31
    // (ac3::bin_to_band(37) == 31), the one case where a channel's own start
    // band is not 0, which is exactly where an absolute-band-0 cursor and a
    // channel-relative one diverge. A `cursor = 0` bug emits deltoffst[0] ==
    // 32 (the absolute band number); the fix emits deltoffst[0] == 1 (band
    // 32's offset from bndstrt == 31).
    constexpr int kStart = 37;
    constexpr int kEnd = 46;  // bands 31, 32, 33 (Table 7.12: 3 bins each)
    constexpr int kExp = 10;
    std::vector<std::uint8_t> exps(kEnd, 0);
    std::fill(exps.begin() + kStart, exps.end(), static_cast<std::uint8_t>(kExp));
    const double baseline = std::pow(2.0, -1.0 - kExp);
    std::vector<double> coeffs(kEnd, 0.0);
    std::fill(coeffs.begin() + kStart, coeffs.end(), baseline);
    // Boost band 32 only (bins 40..42): +6 dB, one Table 5.17 step.
    for (int bin = 40; bin < 43; ++bin) {
        coeffs[static_cast<std::size_t>(bin)] = baseline * 2.0;
    }
    const auto segs = ac3::choose_delta_segments(coeffs, exps, kStart);
    REQUIRE(segs.deltnseg == 1);
    CHECK(segs.deltoffst[0] == 1);
    CHECK(segs.deltlen[0] == 1);
    CHECK(segs.deltba[0] == 4);  // +6 dB
}

TEST_CASE("compute_bit_allocation applies delta at the coupling channel's own "
         "bands, not band 0",
         "[bitalloc]") {
    // Same shape bug as the choose_delta_segments case above, checked from
    // the decoder side: a delta segment addressed at the coupling channel's
    // OWN first band (offset 0 from bndstrt) must change that band's bap -
    // under a `band = 0` bug it lands on mask[0..], which this channel's
    // masking-curve loop never populates and the final bap loop (which
    // starts at bndstrt) never reads, so the correction would silently do
    // nothing and bap_with_delta would equal bap_without_delta.
    constexpr int kStart = 37;   // bndstrt == 31
    constexpr int kEnd = 121;
    constexpr int kExp = 10;     // loud enough that mask perturbation moves bap
    std::vector<std::uint8_t> exps(kEnd, 0);
    std::fill(exps.begin() + kStart, exps.end(), static_cast<std::uint8_t>(kExp));
    const ac3::BitAllocCodes codes{};
    const ac3::BitAllocRegion base_region{
        .start = kStart, .coupling = true, .cplfleak = 3, .cplsleak = 3};
    std::vector<std::uint8_t> bap_without(kEnd);
    ac3::compute_bit_allocation(exps, ac3::SampleRate::k48000, codes, 22, 9, bap_without,
                                base_region);

    ac3::BitAllocRegion delta_region = base_region;
    delta_region.delta = {.deltnseg = 1,
                          .deltoffst = {0},
                          .deltlen = {3},
                          .deltba = {7}};  // +24 dB, at bndstrt's own first 3 bands
    std::vector<std::uint8_t> bap_with(kEnd);
    ac3::compute_bit_allocation(exps, ac3::SampleRate::k48000, codes, 22, 9, bap_with,
                                delta_region);

    CHECK(bap_with != bap_without);
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
