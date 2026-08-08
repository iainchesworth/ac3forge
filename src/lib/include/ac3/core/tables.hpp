#pragma once

#include <array>
#include <cstdint>
#include <optional>

// Base constants of the AC-3 syntax, transcribed from ATSC A/52:2018.
// Every entry cites the section or table it comes from; nothing here is
// derived from any third-party implementation.

namespace ac3 {

// A/52 §5.4.1.1: every syncframe begins with this 16-bit sync word.
inline constexpr std::uint16_t kSyncWord = 0x0B77;

// A/52 §4.1: a syncframe carries 6 audio blocks of 256 samples per channel.
inline constexpr int kBlocksPerFrame = 6;
// §7.3.1: the LFE channel always codes exactly 7 mantissas.
inline constexpr int kLfeEndmant = 7;
inline constexpr int kSamplesPerBlock = 256;
inline constexpr int kSamplesPerFrame = kBlocksPerFrame * kSamplesPerBlock;  // 1536

// A/52 §5.4.1.3, Table 5.6: fscod — the 2-bit sample-rate code ('11' reserved).
enum class SampleRate : std::uint8_t {
    k48000 = 0,
    k44100 = 1,
    k32000 = 2,
};

[[nodiscard]] constexpr std::uint32_t sample_rate_hz(SampleRate sr) {
    switch (sr) {
        case SampleRate::k48000: return 48000;
        case SampleRate::k44100: return 44100;
        case SampleRate::k32000: return 32000;
    }
    return 0;
}

// A/52 §5.4.2.3, Table 5.8: acmod — the 3-bit audio coding mode. Enumerator
// values are the field values; names follow the spec's front/rear notation.
enum class Acmod : std::uint8_t {
    kDualMono = 0,  // 1+1: two independent programs (Ch1, Ch2)
    k1_0 = 1,       // C
    k2_0 = 2,       // L, R
    k3_0 = 3,       // L, C, R
    k2_1 = 4,       // L, R, S
    k3_1 = 5,       // L, C, R, S
    k2_2 = 6,       // L, R, SL, SR
    k3_2 = 7,       // L, C, R, SL, SR
};

// Full-bandwidth channel count (nfchans) per Table 5.8. LFE is additional.
[[nodiscard]] constexpr int fullbw_channel_count(Acmod acmod) {
    constexpr std::array<int, 8> counts = {2, 1, 2, 3, 3, 4, 4, 5};
    return counts[static_cast<std::uint8_t>(acmod)];
}

// A/52 §7.1.3, Table 7.4: exponent strategy codes for chexpstr/cplexpstr.
// Block 0 shall not use kReuse (§5.4.3.22).
enum class ExpStrategy : std::uint8_t {
    kReuse = 0,
    kD15 = 1,
    kD25 = 2,
    kD45 = 3,
};

// A/52 Table 5.18: the 19 nominal bit rates. frmsizecod / 2 indexes this list.
inline constexpr std::array<std::uint16_t, 19> kBitratesKbps = {
    32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 384, 448, 512, 576, 640,
};

[[nodiscard]] constexpr std::optional<int> bitrate_index(std::uint32_t kbps) {
    for (std::size_t i = 0; i < kBitratesKbps.size(); ++i) {
        if (kBitratesKbps[i] == kbps) {
            return static_cast<int>(i);
        }
    }
    return std::nullopt;
}

[[nodiscard]] constexpr bool is_valid_bitrate(std::uint32_t kbps) {
    return bitrate_index(kbps).has_value();
}

// A/52 Table 5.18 "Frame Size Code Table (1 word = 16 bits)", transcribed
// verbatim. Row = bit-rate index; column = fscod (48 / 44.1 / 32 kHz). The
// table lists two frmsizecod values per bit rate: at 44.1 kHz the odd code is
// one word longer (the padding word CBR streams alternate to hit the exact
// rate); at 32 and 48 kHz both codes give the identical size shown here.
inline constexpr std::array<std::array<std::uint16_t, 3>, 19> kFrameSizeWords = {{
    // 48 kHz  44.1 kHz  32 kHz
    {64, 69, 96},          // 32 kbps
    {80, 87, 120},         // 40 kbps
    {96, 104, 144},        // 48 kbps
    {112, 121, 168},       // 56 kbps
    {128, 139, 192},       // 64 kbps
    {160, 174, 240},       // 80 kbps
    {192, 208, 288},       // 96 kbps
    {224, 243, 336},       // 112 kbps
    {256, 278, 384},       // 128 kbps
    {320, 348, 480},       // 160 kbps
    {384, 417, 576},       // 192 kbps
    {448, 487, 672},       // 224 kbps
    {512, 557, 768},       // 256 kbps
    {640, 696, 960},       // 320 kbps
    {768, 835, 1152},      // 384 kbps
    {896, 975, 1344},      // 448 kbps
    {1024, 1114, 1536},    // 512 kbps
    {1152, 1253, 1728},    // 576 kbps
    {1280, 1393, 1920},    // 640 kbps
}};

namespace detail {
// A 1536-sample frame spans exactly 32 ms at 48 kHz and 48 ms at 32 kHz, so
// those Table 5.18 columns must equal the closed-form kbps*2 / kbps*3 words.
consteval bool frame_table_matches_closed_form() {
    for (std::size_t i = 0; i < kBitratesKbps.size(); ++i) {
        if (kFrameSizeWords[i][0] != kBitratesKbps[i] * 2) return false;
        if (kFrameSizeWords[i][2] != kBitratesKbps[i] * 3) return false;
    }
    return true;
}
static_assert(frame_table_matches_closed_form());
}  // namespace detail

// Words per syncframe (A/52 Table 5.18). pad441 selects the odd frmsizecod,
// which adds one word at 44.1 kHz only.
[[nodiscard]] constexpr std::optional<std::uint32_t> frame_size_words(SampleRate sr,
                                                                      std::uint32_t bitrate_kbps,
                                                                      bool pad441 = false) {
    const auto idx = bitrate_index(bitrate_kbps);
    if (!idx) {
        return std::nullopt;
    }
    std::uint32_t words = kFrameSizeWords[static_cast<std::size_t>(*idx)]
                                         [static_cast<std::uint8_t>(sr)];
    if (sr == SampleRate::k44100 && pad441) {
        words += 1;
    }
    return words;
}

[[nodiscard]] constexpr std::optional<std::uint32_t> frame_size_bytes(SampleRate sr,
                                                                      std::uint32_t bitrate_kbps,
                                                                      bool pad441 = false) {
    const auto words = frame_size_words(sr, bitrate_kbps, pad441);
    if (!words) {
        return std::nullopt;
    }
    return *words * 2;
}

// A/52 §7.10.1: the number of words in the first 5/8 of the syncframe —
// the region protected by crc1 (sync word included in the count but excluded
// from the CRC itself).
[[nodiscard]] constexpr std::uint32_t frame_size_58_words(std::uint32_t frame_words) {
    return (frame_words >> 1) + (frame_words >> 3);
}

}  // namespace ac3
