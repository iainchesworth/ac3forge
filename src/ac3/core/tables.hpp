#pragma once

#include <array>
#include <cstdint>
#include <optional>

// Base constants of the AC-3 syntax, transcribed from ATSC A/52:2018.
// Every entry cites the section or table it comes from; nothing here is
// derived from any third-party implementation.

namespace ac3 {

// A/52 §5.3.1: every syncframe begins with this 16-bit sync word.
inline constexpr std::uint16_t kSyncWord = 0x0B77;

// A/52 §4.1: a syncframe carries 6 audio blocks of 256 samples per channel.
inline constexpr int kBlocksPerFrame = 6;
inline constexpr int kSamplesPerBlock = 256;
inline constexpr int kSamplesPerFrame = kBlocksPerFrame * kSamplesPerBlock;  // 1536

// A/52 §5.4.1.3: fscod — the 2-bit sample-rate code ('11' is reserved).
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

// A/52 §5.4.2.3: acmod — the 3-bit audio coding mode. Enumerator values are
// the field values; names follow the spec's front/rear notation.
enum class Acmod : std::uint8_t {
    kDualMono = 0,  // 1+1: two independent programs
    k1_0 = 1,       // C
    k2_0 = 2,       // L R
    k3_0 = 3,       // L C R
    k2_1 = 4,       // L R S
    k3_1 = 5,       // L C R S
    k2_2 = 6,       // L R SL SR
    k3_2 = 7,       // L C R SL SR
};

// Full-bandwidth channel count for an acmod (LFE, if present, is additional).
[[nodiscard]] constexpr int fullbw_channel_count(Acmod acmod) {
    constexpr std::array<int, 8> counts = {2, 1, 2, 3, 3, 4, 4, 5};
    return counts[static_cast<std::uint8_t>(acmod)];
}

// A/52 Table 5.18: the 19 legal bit rates. frmsizecod / 2 indexes this list;
// at 44.1 kHz the low bit of frmsizecod additionally selects the padded frame
// length (see frame_size_bytes below).
inline constexpr std::array<std::uint16_t, 19> kBitratesKbps = {
    32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 384, 448, 512, 576, 640,
};

[[nodiscard]] constexpr bool is_valid_bitrate(std::uint32_t kbps) {
    for (auto rate : kBitratesKbps) {
        if (rate == kbps) {
            return true;
        }
    }
    return false;
}

// Frame size per A/52 Table 5.18. A frame spans 1536 samples, which is exactly
// 32 ms at 48 kHz and 48 ms at 32 kHz, so those sizes follow from the bit rate
// with no rounding (e.g. 640 kbit/s @ 48 kHz = 2560 bytes). 44.1 kHz frames
// are non-integral in bytes and alternate one padding word between adjacent
// frmsizecod values; that column will be transcribed verbatim from Table 5.18
// when the framer lands (Milestone 1) rather than approximated here.
[[nodiscard]] constexpr std::optional<std::uint32_t> frame_size_bytes(SampleRate sr,
                                                                      std::uint32_t bitrate_kbps) {
    if (!is_valid_bitrate(bitrate_kbps)) {
        return std::nullopt;
    }
    switch (sr) {
        case SampleRate::k48000: return bitrate_kbps * 4;  // kbps * 32 ms / 8
        case SampleRate::k32000: return bitrate_kbps * 6;  // kbps * 48 ms / 8
        case SampleRate::k44100: return std::nullopt;      // Table 5.18 transcription pending
    }
    return std::nullopt;
}

}  // namespace ac3
