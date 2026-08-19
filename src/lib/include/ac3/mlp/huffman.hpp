#pragma once

#include <array>
#include <cstdint>
#include <span>

#include "ac3/core/bitreader.hpp"
#include "ac3/core/bitwriter.hpp"
#include "ac3/export.hpp"

// MLP's entropy layer, transcribed from WO 96/37048 (Craven & Gerzon,
// "Lossless coding method for waveform data", PCT filed 1996-05-15) - the
// document Dolby's own US 7,193,538 B2 names as a source of "a description
// of MLP" and whose processes it says the MLP lossless cores are
// "implemented according to" in preferred embodiments. Table and figure
// numbers below are the WO's own. This replaces rice.hpp's Golomb-Rice
// stand-in as the real mechanism: block-adaptive prefix-code tables, not a
// parameterised Rice code.
//
// What is transcription and what is our own choice is kept explicit:
//   - The CODEWORDS (Tables 2, 4, 5, 6), the 17-table selection-by-peak
//     rule and its ranges (Table 3), the top-4-varying-digits split, and
//     the PCM fallback (Table 7) are transcribed from the WO.
//   - The exact binary convention mapping a signed significant word onto
//     "digits" (we use offset-binary: u = x + 2^n - 1, which reproduces
//     Table 3's asymmetric ranges exactly) is OUR self-consistent reading;
//     the shipping format's literal bit convention is a format-layer detail
//     the WO does not pin down (see docs/concepts/truehd-mlp.md, layer 3).
//     Internal encode/decode round-trips are exact either way.

namespace ac3::mlp::huffman {

// --- Table 2: the Laplacian 4-bit code ------------------------------------
//
// WO Table 2, all sixteen codewords, for values -7..+8: a run of 0s
// terminated by 1 on the negative side (0 itself is "01"), a run of 1s
// terminated by 0 on the positive side, with the two 8-bit extremes -7
// ("00000000") and +8 ("11111111") having no terminator. Tuned for
// Laplacian statistics at block length L >= 256 (WO Fig. 19).
inline constexpr int kTable2Min = -7;
inline constexpr int kTable2Max = 8;

[[nodiscard]] constexpr int table2_length(int value) {
    return (value == kTable2Min || value == kTable2Max) ? 8
           : value <= 0                                 ? 2 - value
                                                        : value + 1;
}

AC3FORGE_EXPORT void encode_table2(BitWriter& w, int value);
[[nodiscard]] AC3FORGE_EXPORT int decode_table2(BitReader& r);

// --- Table 3: the seventeen peak-level-selected tables ---------------------
//
// WO Table 3: for a block whose significant words all lie in
// -2^n + 1 <= x <= 2^n (n = 3..19), the top FOUR varying digits are coded
// with Table 2 and the remaining n-3 digits follow raw, immediately after
// the codeword - total n+1 .. n+7 bits per sample. "The one used depends on
// the peak signal level in a block." Seventeen values of n = seventeen
// tables. Quoted inefficiency vs optimal Huffman coding: ~0.2 bit/sample.
inline constexpr int kMinN = 3;
inline constexpr int kMaxN = 19;

// The smallest n whose Table 3 range holds every value in [lo, hi].
// Asserts the values fit table 17's range at all (a 24-bit significant
// word with B1 = 0 can exceed it - the WO's worked example is N = 20;
// callers with wider words fall back to encode_pcm below).
[[nodiscard]] AC3FORGE_EXPORT int select_n(std::int32_t lo, std::int32_t hi);

AC3FORGE_EXPORT void encode_significant(BitWriter& w, std::int32_t x, int n);
[[nodiscard]] AC3FORGE_EXPORT std::int32_t decode_significant(BitReader& r, int n);

[[nodiscard]] constexpr int significant_length(std::int32_t x, int n) {
    const auto u = static_cast<std::uint32_t>(x + (std::int32_t{1} << n) - 1);
    return table2_length(static_cast<int>(u >> (n - 3)) - 7) + (n - 3);
}

// --- Tables 4-6: small-signal codes ----------------------------------------
//
// WO: "For cases where the significant word wordlength is smaller than 3
// bits one may use more efficient Huffman codes than the ones in the tables
// 2 and 3 above". Codewords transcribed exactly; the WO prints the range
// headers with strict bounds but the listed entries are inclusive.
struct SmallCode {
    std::int8_t value;
    std::uint8_t code;
    std::uint8_t length;
};

// Table 4: -1..+2.
inline constexpr std::array<SmallCode, 4> kTable4{{
    {-1, 0b111, 3},
    {0, 0b0, 1},
    {1, 0b10, 2},
    {2, 0b110, 3},
}};

// Table 5: -2..+2.
inline constexpr std::array<SmallCode, 5> kTable5{{
    {-2, 0b110, 3},
    {-1, 0b100, 3},
    {0, 0b0, 1},
    {1, 0b101, 3},
    {2, 0b111, 3},
}};

// Table 6: -3..+3.
inline constexpr std::array<SmallCode, 7> kTable6{{
    {-3, 0b1100, 4},
    {-2, 0b1101, 4},
    {-1, 0b100, 3},
    {0, 0b0, 1},
    {1, 0b101, 3},
    {2, 0b1110, 4},
    {3, 0b1111, 4},
}};

AC3FORGE_EXPORT void encode_small(BitWriter& w, std::span<const SmallCode> table, int value);
[[nodiscard]] AC3FORGE_EXPORT int decode_small(BitReader& r, std::span<const SmallCode> table);

// --- Table 7: the PCM code -------------------------------------------------
//
// WO Table 7: for -2^n + 1 <= x <= 2^n, transmit all n+1 digits raw - "a
// Huffman coding (optimum for uniform PDF signal statistics)". "usually
// less efficient ... but is occasionally more efficient (e.g. on sine wave
// signals) and has the unique property of isolated data errors not
// affecting the decoding of the rest of the Huffman coded waveform
// sequence for a block". Unlike Table 3, n is not capped at 19: PCM covers
// any width up to a 24-bit significant word.
AC3FORGE_EXPORT void encode_pcm(BitWriter& w, std::int32_t x, int n);
[[nodiscard]] AC3FORGE_EXPORT std::int32_t decode_pcm(BitReader& r, int n);

// The WO also defines an "empty" table for digital-black blocks - a block
// of all zeros sends NO waveform data at all, and "predictor filter
// coefficients and initialisation data need not be transmitted". That is a
// block-header-level decision (which table number to write), not an
// encoder here: encoding nothing needs no function.

}  // namespace ac3::mlp::huffman
