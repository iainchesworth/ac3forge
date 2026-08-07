#include "ac3/sinks/iec61937.hpp"

#include "ac3/core/tables.hpp"

namespace ac3::iec61937 {

namespace {

void put_word_le(std::vector<std::byte>& out, std::uint16_t word) {
    out.push_back(static_cast<std::byte>(word & 0xFF));
    out.push_back(static_cast<std::byte>(word >> 8));
}

}  // namespace

std::expected<std::vector<std::byte>, WrapError> wrap_frame(std::span<const std::byte> frame) {
    if (frame.size() < 6 || (frame.size() & 1) != 0 ||
        std::to_integer<std::uint8_t>(frame[0]) != 0x0B ||
        std::to_integer<std::uint8_t>(frame[1]) != 0x77) {
        return std::unexpected(WrapError::kNotAFrame);
    }
    if (frame.size() + 8 > kBurstBytes) {
        return std::unexpected(WrapError::kFrameTooLarge);
    }
    // bsmod: the 3 bits after the 5-bit bsid in byte 5.
    const auto bsmod = std::to_integer<std::uint16_t>(frame[5]) & 0x7;

    std::vector<std::byte> burst;
    burst.reserve(kBurstBytes);
    put_word_le(burst, 0xF872);  // Pa
    put_word_le(burst, 0x4E1F);  // Pb
    put_word_le(burst, static_cast<std::uint16_t>(1 | (bsmod << 8)));  // Pc: type 1 = AC-3
    put_word_le(burst, static_cast<std::uint16_t>(frame.size() * 8));  // Pd: bits
    for (std::size_t i = 0; i < frame.size(); i += 2) {
        // Frame bytes big-endian within each word, emitted little-endian.
        put_word_le(burst, static_cast<std::uint16_t>(
                               (std::to_integer<std::uint16_t>(frame[i]) << 8) |
                               std::to_integer<std::uint16_t>(frame[i + 1])));
    }
    burst.resize(kBurstBytes, std::byte{0});
    return burst;
}

}  // namespace ac3::iec61937
