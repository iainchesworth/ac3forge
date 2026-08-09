#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <vector>

// IEC 61937 ("S/PDIF burst") packing: an AC-3 syncframe disguised as 16-bit
// stereo PCM so AV receivers accept it over S/PDIF or HDMI. Each burst is
// exactly 6144 bytes (1536 stereo 16-bit sample frames — one AC-3 frame
// duration at any AC-3 sample rate): the four preamble words Pa 0xF872,
// Pb 0x4E1F, Pc (data type 1 = AC-3, with bsmod in bits 8..10), Pd (payload
// length in BITS), then the frame bytes packed big-endian into words, zero-
// padded to the burst length. Words are emitted little-endian, ready for a
// PCM16 container; byte-exact against FFmpeg's spdif muxer as the oracle.

namespace ac3::iec61937 {

inline constexpr std::size_t kBurstBytes = 6144;

enum class WrapError : std::uint8_t {
    kNotAFrame,       // missing sync word or truncated header
    kFrameTooLarge,   // cannot happen for legal AC-3 sizes; guarded anyway
};

// Wrap exactly one AC-3 syncframe into one 6144-byte burst.
[[nodiscard]] std::expected<std::vector<std::byte>, WrapError> wrap_frame(
    std::span<const std::byte> frame);

}  // namespace ac3::iec61937
