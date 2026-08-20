#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <numeric>
#include <vector>

#include "ac3/encoder/eac3_tools.hpp"

using namespace ac3::eac3;

TEST_CASE("ecpl_begin_subbnd matches every row of Table E3.8", "[enhanced_coupling]") {
    // {sub-band #, ecplbegf} - every row of the table that carries a begf code.
    struct Row {
        int sbnd;
        int ecplbegf;
    };
    constexpr std::array<Row, 15> kRows = {{
        {0, 0}, {2, 1}, {4, 2}, {5, 3}, {6, 4}, {7, 5}, {8, 6}, {9, 7}, {10, 8}, {11, 9},
        {12, 10}, {13, 11}, {16, 13}, {18, 14}, {20, 15},
    }};
    for (const auto& row : kRows) {
        CAPTURE(row.ecplbegf, row.sbnd);
        CHECK(ecpl_begin_subbnd(row.ecplbegf) == row.sbnd);
    }
}

TEST_CASE("ecpl_end_subbnd matches every row of Table E3.8", "[enhanced_coupling]") {
    // {sub-band #, ecplendf} - every row that carries an endf code. The
    // table's sub-band# column IS ecpl_end_subbnd's value for these rows.
    struct Row {
        int sbnd;
        int ecplendf;
    };
    constexpr std::array<Row, 16> kRows = {{
        {7, 0}, {8, 1}, {9, 2}, {10, 3}, {11, 4}, {12, 5}, {13, 6}, {14, 7}, {15, 8}, {16, 9},
        {17, 10}, {18, 11}, {19, 12}, {20, 13}, {21, 14}, {22, 15},
    }};
    for (const auto& row : kRows) {
        CAPTURE(row.ecplendf, row.sbnd);
        CHECK(ecpl_end_subbnd(row.ecplendf) == row.sbnd);
    }
}

TEST_CASE("ecpl_end_subbnd_from_spx matches Table E3.8's SPX-derived branch",
          "[enhanced_coupling]") {
    // §E2.3.3.17: spxbegf < 6 -> spxbegf + 5, else spxbegf * 2. Cross-checked
    // against spx_begin_subbnd's own table rather than transcribed twice.
    CHECK(ecpl_end_subbnd_from_spx(0) == 5);
    CHECK(ecpl_end_subbnd_from_spx(5) == 10);
    CHECK(ecpl_end_subbnd_from_spx(6) == 12);
    CHECK(ecpl_end_subbnd_from_spx(9) == 18);
}

TEST_CASE("enhanced coupling sub-band table matches Table E3.7", "[enhanced_coupling]") {
    STATIC_CHECK(kEcplSubBandTab[0] == 13);
    STATIC_CHECK(kEcplSubBandTab[22] == 253);
    // Sub-bands 0-3 are 6 bins wide (unlike standard coupling's uniform 12);
    // 4-21 are 12.
    for (int sbnd = 0; sbnd < kEcplSubBands; ++sbnd) {
        const int width = kEcplSubBandTab[static_cast<std::size_t>(sbnd) + 1] -
                          kEcplSubBandTab[static_cast<std::size_t>(sbnd)];
        CAPTURE(sbnd, width);
        CHECK(width == (sbnd < 4 ? 6 : 12));
    }
}

TEST_CASE("ecpl_group_bands tiles the coupled region exactly once", "[enhanced_coupling]") {
    // Same invariant as standard coupling's "bands tile the coupled region
    // exactly once", but over the non-uniform sub-band widths: no gap goes
    // uncoded, no bin is covered twice, and structure[begin] is never
    // consulted (the first sub-band always opens a band).
    for (int begin = 0; begin < kEcplSubBands; ++begin) {
        for (int end = begin + 1; end <= kEcplSubBands; ++end) {
            CAPTURE(begin, end);
            const auto bands = ecpl_group_bands(begin, end, kDefaultEcplBandStructure);
            REQUIRE(bands.count >= 1);
            REQUIRE(bands.count <= end - begin);

            int bin = kEcplSubBandTab[static_cast<std::size_t>(begin)];
            for (int bnd = 0; bnd < bands.count; ++bnd) {
                CAPTURE(bnd);
                CHECK(bands.start[static_cast<std::size_t>(bnd)] == bin);
                bin += bands.size[static_cast<std::size_t>(bnd)];
            }
            CHECK(bin == kEcplSubBandTab[static_cast<std::size_t>(end)]);
        }
    }
}

TEST_CASE("default enhanced coupling band structure matches Table E2.13",
          "[enhanced_coupling]") {
    // Sub-bands 0-8 never merge (§E2.3.3.19: their bits are never even
    // transmitted); above that the default folds most sub-bands together.
    for (int sbnd = 0; sbnd <= 8; ++sbnd) {
        CAPTURE(sbnd);
        CHECK_FALSE(kDefaultEcplBandStructure[static_cast<std::size_t>(sbnd)]);
    }
    constexpr std::array<bool, 13> kExpectedFrom9 = {
        true, false, true, false, true, false, true, true, true, false, true, true, true,
    };
    for (std::size_t i = 0; i < kExpectedFrom9.size(); ++i) {
        const auto sbnd = 9 + i;
        CAPTURE(sbnd);
        CHECK(kDefaultEcplBandStructure[sbnd] == kExpectedFrom9[i]);
    }
}

TEST_CASE("decode_ecplamp follows Table E3.10, index 31 is silence", "[enhanced_coupling]") {
    // Index 0: exp 0, mant 0x20 -> (32/32) >> 0 == 1.0 (0 dB, unity gain).
    CHECK(decode_ecplamp(0) == 1.0);
    // Index 4: exp 0, mant 0x10 -> (16/32) >> 0 == 0.5.
    CHECK(decode_ecplamp(4) == 0.5);
    // Index 8: exp 1, mant 0x10 -> 0.5 >> 1 == 0.25.
    CHECK(decode_ecplamp(8) == 0.25);
    // Index 31 is the "-infinity dB" special case, not a table lookup.
    CHECK(decode_ecplamp(31) == 0.0);
    // Monotonically non-increasing across the real entries.
    for (int i = 1; i < 31; ++i) {
        CAPTURE(i);
        CHECK(decode_ecplamp(i) <= decode_ecplamp(i - 1));
    }
}

TEST_CASE("enhanced coupling coordinate quantizers round-trip their own table",
          "[enhanced_coupling]") {
    // Quantizing a value the table can already represent exactly must land
    // back on the same index - the property that matters for the encoder,
    // which always starts from one of these table values when reusing a
    // previous block's coordinate.
    for (int i = 0; i < 31; ++i) {
        CAPTURE(i);
        CHECK(quantize_ecplamp(decode_ecplamp(i)) == i);
    }
    CHECK(quantize_ecplamp(0.0) == 31);
    CHECK(quantize_ecplamp(-1.0) == 31);

    for (int i = 0; i < 64; ++i) {
        CAPTURE(i);
        CHECK(quantize_ecplangle(decode_ecplangle(i)) == i);
    }
    // The angle table represents a wrapped [-1, 1) phase; values outside that
    // range must wrap rather than clamp, since angle arithmetic in §3.5.5.3
    // relies on exactly that.
    CHECK(quantize_ecplangle(1.0) == quantize_ecplangle(-1.0));

    for (int i = 0; i < 8; ++i) {
        CAPTURE(i);
        CHECK(quantize_ecplchaos(decode_ecplchaos(i)) == i);
    }
}

TEST_CASE("decode_ecplangle and decode_ecplchaos match their closed forms",
          "[enhanced_coupling]") {
    // Both tables are exact linear ramps (§3.5.4's -1.0..0.96875 and
    // 0.0..-1.0), verified here against the boundary values Table E3.11 and
    // Table E3.12 print explicitly.
    STATIC_CHECK(decode_ecplangle(0) == 0.0);
    STATIC_CHECK(decode_ecplangle(16) == 0.5);
    STATIC_CHECK(decode_ecplangle(31) == 31.0 / 32.0);
    STATIC_CHECK(decode_ecplangle(32) == -1.0);
    STATIC_CHECK(decode_ecplangle(63) == -1.0 / 32.0);

    STATIC_CHECK(decode_ecplchaos(0) == 0.0);
    CHECK(decode_ecplchaos(1) == Catch::Approx(-0.142857).epsilon(1e-5));
    CHECK(decode_ecplchaos(7) == -1.0);
}

TEST_CASE("ecpl_channel_spectrum of all-zero neighbors is all zero", "[enhanced_coupling]") {
    std::array<double, 256> zero{};
    std::array<double, 256> real_out{};
    std::array<double, 256> imag_out{};
    real_out.fill(1.0);  // poison, so the function must actually write zero
    imag_out.fill(1.0);
    ecpl_channel_spectrum(zero, zero, zero, real_out, imag_out);
    for (int k = 0; k < 256; ++k) {
        CAPTURE(k);
        CHECK(real_out[static_cast<std::size_t>(k)] == Catch::Approx(0.0).margin(1e-9));
        CHECK(imag_out[static_cast<std::size_t>(k)] == Catch::Approx(0.0).margin(1e-9));
    }
}

TEST_CASE("ecpl_channel_spectrum is deterministic", "[enhanced_coupling]") {
    std::array<double, 256> prev{};
    std::array<double, 256> curr{};
    std::array<double, 256> next{};
    prev[20] = 0.4;
    curr[20] = 1.0;
    curr[100] = -0.3;
    next[20] = -0.5;
    std::array<double, 256> re1{}, im1{}, re2{}, im2{};
    ecpl_channel_spectrum(prev, curr, next, re1, im1);
    ecpl_channel_spectrum(prev, curr, next, re2, im2);
    CHECK(re1 == re2);
    CHECK(im1 == im2);
    // And not trivially all-zero for non-zero input.
    const bool any_nonzero =
        std::any_of(re1.begin(), re1.end(), [](double v) { return std::abs(v) > 1e-9; }) ||
        std::any_of(im1.begin(), im1.end(), [](double v) { return std::abs(v) > 1e-9; });
    CHECK(any_nonzero);
}

TEST_CASE("ecpl_rand_notrans is uniform-shaped, deterministic and bin/channel-distinct",
          "[enhanced_coupling]") {
    for (int bin = 0; bin < 256; ++bin) {
        CAPTURE(bin);
        const double v = ecpl_rand_notrans(0, bin);
        CHECK(v >= -1.0);
        CHECK(v <= 1.0);
        CHECK(ecpl_rand_notrans(0, bin) == v);  // deterministic, same call twice
    }
    // Distinct bins should (overwhelmingly) not collide.
    std::vector<double> values;
    for (int bin = 0; bin < 32; ++bin) {
        values.push_back(ecpl_rand_notrans(1, bin));
    }
    std::ranges::sort(values);
    CHECK(std::adjacent_find(values.begin(), values.end()) == values.end());
}

TEST_CASE("EcplNoise::next is uniform-shaped and advances", "[enhanced_coupling]") {
    EcplNoise noise;
    double first = noise.next();
    CHECK(first >= -1.0);
    CHECK(first <= 1.0);
    bool ever_different = false;
    for (int i = 0; i < 16; ++i) {
        const double v = noise.next();
        CHECK(v >= -1.0);
        CHECK(v <= 1.0);
        if (v != first) {
            ever_different = true;
        }
    }
    CHECK(ever_different);
}

TEST_CASE("ecpl_amplitudes duplicates one value per band across its sub-bands' bins",
          "[enhanced_coupling]") {
    // Two bands: sub-band 9 alone, then 10-11 merged (structure[11] set).
    // begin_subbnd=9, end_subbnd=12 -> covers sub-bands 9,10,11.
    std::array<bool, kEcplSubBands> structure{};
    structure[11] = true;  // merge sub-band 11 into 10's band
    const std::array<int, 2> amp = {0, 4};    // band0 -> decode_ecplamp(0)=1.0, band1 -> 0.5
    const std::array<int, 2> chaos = {0, 0};  // irrelevant: is_first_channel silences it
    const int begin = 9;
    const int end = 12;
    const int bins = kEcplSubBandTab[static_cast<std::size_t>(end)] -
                     kEcplSubBandTab[static_cast<std::size_t>(begin)];
    std::vector<double> out(static_cast<std::size_t>(bins));
    ecpl_amplitudes(amp, chaos, /*ecpltrans=*/false, /*is_first_channel=*/true, begin, end,
                    structure, out);

    const int sb9_width =
        kEcplSubBandTab[10] - kEcplSubBandTab[9];  // band 0: sub-band 9 alone
    for (int i = 0; i < sb9_width; ++i) {
        CAPTURE(i);
        CHECK(out[static_cast<std::size_t>(i)] == 1.0);
    }
    for (int i = sb9_width; i < bins; ++i) {
        CAPTURE(i);
        CHECK(out[static_cast<std::size_t>(i)] == 0.5);
    }
}

TEST_CASE("ecpl_amplitudes applies the chaos modification only for a non-first, non-transient channel",
          "[enhanced_coupling]") {
    std::array<bool, kEcplSubBands> structure{};
    const std::array<int, 1> amp = {0};  // decode_ecplamp(0) == 1.0
    const std::array<int, 1> chaos = {7};  // decode_ecplchaos(7) == -1.0
    const int begin = 9;
    const int end = 10;
    const int bins = kEcplSubBandTab[10] - kEcplSubBandTab[9];
    std::vector<double> out(static_cast<std::size_t>(bins));

    // First channel: chaos never applies, whatever ecpltrans says.
    ecpl_amplitudes(amp, chaos, false, /*is_first_channel=*/true, begin, end, structure, out);
    CHECK(out[0] == 1.0);

    // Non-first, no transient: 1.0 * (1 + 0.38 * -1.0) == 0.62.
    ecpl_amplitudes(amp, chaos, false, /*is_first_channel=*/false, begin, end, structure, out);
    CHECK(out[0] == Catch::Approx(0.62).epsilon(1e-9));

    // Non-first, WITH a transient: chaos modification is skipped.
    ecpl_amplitudes(amp, chaos, true, /*is_first_channel=*/false, begin, end, structure, out);
    CHECK(out[0] == 1.0);
}

TEST_CASE("ecpl_angles is identically zero for the first coupled channel", "[enhanced_coupling]") {
    // §3.5.5.3: angle[ch][bnd] = 0 when ch == firstchincpl, and chaos is also
    // zero there, so the de-correlation add contributes nothing either -
    // every bin comes out exactly 0 regardless of the transmitted fields.
    std::array<bool, kEcplSubBands> structure{};
    const std::array<int, 2> angle = {63, 1};
    const std::array<int, 2> chaos = {7, 7};
    const int begin = 9;
    const int end = 11;
    const int bins = kEcplSubBandTab[11] - kEcplSubBandTab[9];
    std::vector<double> out(static_cast<std::size_t>(bins));
    EcplNoise noise;
    ecpl_angles(/*channel=*/0, angle, chaos, /*ecpltrans=*/true, /*is_first_channel=*/true, begin,
               end, structure, noise, out);
    for (int i = 0; i < bins; ++i) {
        CAPTURE(i);
        CHECK(out[static_cast<std::size_t>(i)] == 0.0);
    }
}

TEST_CASE("ecpl_channel_coefficients: zero amplitude silences a channel", "[enhanced_coupling]") {
    std::array<double, 256> real_in{};
    std::array<double, 256> imag_in{};
    real_in[20] = 5.0;
    imag_in[20] = -3.0;
    const std::array<double, 1> amp = {0.0};
    const std::array<double, 1> angle = {0.0};
    std::array<double, 256> mant_out{};
    mant_out.fill(1.0);  // poison
    ecpl_channel_coefficients(real_in, imag_in, amp, angle, 20, 21, mant_out);
    CHECK(mant_out[20] == 0.0);
}

TEST_CASE("ecpl_channel_coefficients: unity amplitude and zero angle is a plain fold",
          "[enhanced_coupling]") {
    // angle == 0 -> cos(0) == 1, sin(0) == 0, so Zr[ch] == Zr, Zi[ch] == Zi
    // exactly - amp == 1 leaves the complex value untouched, and the
    // formula reduces to -2*(y[bin]*Zr + y[mirror]*Zi).
    std::array<double, 256> real_in{};
    std::array<double, 256> imag_in{};
    real_in[20] = 5.0;
    imag_in[20] = -3.0;
    const std::array<double, 1> amp = {1.0};
    const std::array<double, 1> angle = {0.0};
    std::array<double, 256> mant_out{};
    ecpl_channel_coefficients(real_in, imag_in, amp, angle, 20, 21, mant_out);
    CHECK(mant_out[20] != 0.0);
    // Every other bin must stay untouched (the function only writes
    // [begin_mant, end_mant)).
    for (int bin = 0; bin < 256; ++bin) {
        if (bin == 20) {
            continue;
        }
        CAPTURE(bin);
        CHECK(mant_out[static_cast<std::size_t>(bin)] == 0.0);
    }
}
