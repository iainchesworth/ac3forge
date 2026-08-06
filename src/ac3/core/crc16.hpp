#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace ac3 {

// CRC-16 as used by AC-3 (A/52 §7.10.1): generator polynomial
// x^16 + x^15 + x^2 + 1 (0x8005), MSB-first bit order, initial register 0,
// no reflection, no final XOR. In CRC-catalogue terms this is CRC-16/UMTS
// (check value 0xFEE8 for the ASCII string "123456789").
//
// A/52 defines two CRC words per syncframe: crc1 covers the first 5/8 of the
// frame and crc2 the remainder; each is chosen so that running the covered
// region (sync word excluded for crc1's region start) through this register
// yields zero. The frame-level insertion logic lives with the framer; this
// header is just the register.

namespace detail {

consteval std::array<std::uint16_t, 256> make_crc16_table() {
    std::array<std::uint16_t, 256> table{};
    for (std::uint32_t i = 0; i < 256; ++i) {
        std::uint32_t reg = i << 8;
        for (int bit = 0; bit < 8; ++bit) {
            reg = (reg & 0x8000) != 0 ? ((reg << 1) ^ 0x8005) & 0xFFFF : (reg << 1) & 0xFFFF;
        }
        table[i] = static_cast<std::uint16_t>(reg);
    }
    return table;
}

inline constexpr auto kCrc16Table = make_crc16_table();

}  // namespace detail

[[nodiscard]] constexpr std::uint16_t crc16(std::span<const std::byte> data,
                                            std::uint16_t crc = 0) {
    for (std::byte b : data) {
        const auto index = ((crc >> 8) ^ std::to_integer<std::uint32_t>(b)) & 0xFF;
        crc = static_cast<std::uint16_t>((crc << 8) ^ detail::kCrc16Table[index]);
    }
    return crc;
}

}  // namespace ac3
