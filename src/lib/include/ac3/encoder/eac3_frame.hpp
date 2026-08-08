#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <vector>

#include "ac3/core/tables.hpp"
#include "ac3/encoder/silent_frame.hpp"  // FrameError

// E-AC-3 (Dolby Digital Plus) framing - ATSC A/52:2018 Annex E, bsid 16.
//
// E-AC-3 is not a variant of the AC-3 frame; it is a different container for
// the same coding tools:
//   - syncinfo is ONLY the sync word. There is no crc1, so the GF(2) leading
//     -CRC solver AC-3 needs has no counterpart here.
//   - frmsiz is an arbitrary 11-bit word count rather than a table lookup, so
//     any frame size is directly expressible and the 44.1 kHz padding
//     alternation AC-3 needs disappears.
//   - Exponent strategies and coupling-in-use for EVERY block are hoisted
//     into a frame-level audfrm element ahead of the blocks, and several
//     per-block fields become conditional on frame-level flags.
//
// This first step emits a valid, decodable bsid-16 frame carrying digital
// silence, the same way the AC-3 work started: all SNR offsets zero, which
// §7.2.2.1.1 defines as an all-zero bit allocation, so no mantissa data
// exists and the frame is pure syntax.

namespace ac3::eac3 {

inline constexpr int kBsid = 16;

struct FrameConfig {
    SampleRate sample_rate = SampleRate::k48000;
    std::uint32_t bitrate_kbps = 192;
    Acmod acmod = Acmod::k2_0;
    bool lfe = false;
    int dialnorm = 31;
    int chbwcod = 60;
};

// Words per syncframe at a given rate. E-AC-3 signals the size directly, so
// this is just the exact bit budget rounded to whole 16-bit words.
[[nodiscard]] constexpr std::uint32_t frame_words(SampleRate sample_rate,
                                                  std::uint32_t bitrate_kbps) {
    const std::uint64_t bits = static_cast<std::uint64_t>(bitrate_kbps) * 1000 *
                               kSamplesPerFrame / sample_rate_hz(sample_rate);
    return static_cast<std::uint32_t>(bits / 16);
}

[[nodiscard]] std::expected<std::vector<std::byte>, FrameError> build_silent_frame(
    const FrameConfig& config);

}  // namespace ac3::eac3
