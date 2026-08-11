#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

// Dolby TrueHD (MLP)'s major-sync CRC - "Dolby TrueHD (MLP) high-level
// bitstream description" §4.2.10 / §2.8. Structurally the same table-driven
// MSB-first CRC as core/crc16.hpp (byte input, zero-initialized register,
// no reflection, no final XOR), just a different generator polynomial, so
// this mirrors that header's shape rather than reusing it - the two
// registers aren't interchangeable and MLP is a deliberately parallel module
// (see docs/concepts/truehd-mlp.md's Phase 2 note on not sharing E-AC-3
// types).
//
// §2.8 names two more CRCs this bitstream uses - substream_CRC (§4.6.7) and
// restart_header_CRC (§4.7.2) - plus a separate XOR parity check
// (mlp_tables.hpp's kParityXorConstant). Neither is transcribed here yet:
// both belong with substream_segment()/restart_header(), which aren't built
// until the access-unit-assembly increment lands. substream_CRC in
// particular is NOT this same table-driven shape - §4.6.7 gives an explicit
// bit-serial algorithm (input bits appended at the register's low end,
// polynomial reduction triggered by an overflow bit) that must be
// transcribed literally rather than assumed equivalent to the byte-table
// form.

namespace ac3::mlp {

namespace detail {

// §4.2.10: x^16 + x^5 + x^3 + x^2 + 1. As a table-driven register, the
// implicit x^16 term is the register width itself (core/crc16.hpp's 0x8005
// drops A/52's own leading x^16 the same way); what's left is
// x^5 + x^3 + x^2 + 1 = 0x20 + 0x8 + 0x4 + 0x1 = 0x2D.
inline constexpr std::uint32_t kMajorSyncCrcPolynomial = 0x002D;

consteval std::array<std::uint16_t, 256> make_major_sync_crc_table() {
    std::array<std::uint16_t, 256> table{};
    for (std::uint32_t i = 0; i < 256; ++i) {
        std::uint32_t reg = i << 8;
        for (int bit = 0; bit < 8; ++bit) {
            reg = (reg & 0x8000) != 0 ? ((reg << 1) ^ kMajorSyncCrcPolynomial) & 0xFFFF
                                       : (reg << 1) & 0xFFFF;
        }
        table[i] = static_cast<std::uint16_t>(reg);
    }
    return table;
}

inline constexpr auto kMajorSyncCrcTable = make_major_sync_crc_table();

}  // namespace detail

// §4.2.10: covers major_sync_info() up to but excluding the CRC word itself,
// zero-initialized register - callers pass exactly that span.
[[nodiscard]] constexpr std::uint16_t major_sync_crc(std::span<const std::byte> data) {
    std::uint16_t crc = 0;
    for (std::byte b : data) {
        const auto index = ((crc >> 8) ^ std::to_integer<std::uint32_t>(b)) & 0xFF;
        crc = static_cast<std::uint16_t>((crc << 8) ^ detail::kMajorSyncCrcTable[index]);
    }
    return crc;
}

// §4.7.2: restart_header_CRC, x^8 + x^4 + x^3 + x^2 + 1, zero-initialized
// register. Unlike major_sync_crc above, this is bit-serial rather than
// byte-table-driven: restart_header() ends with ch_assign[], a
// max_matrix_chan-dependent array, so the CRC's covered span is generally
// NOT a whole number of bytes (§3.2.1 only guarantees a restart point's
// *start* falls on a 16-bit boundary, not everything inside it). `data`
// holds ceil(bit_count/8) bytes, MSB first per byte - typically a BitWriter's
// take(), whose own trailing zero-padding bits beyond `bit_count` are
// harmless since this stops reading at exactly `bit_count`.
//
// The document does not give restart_header_CRC an explicit bit-serial
// algorithm the way §4.6.7 does for substream_CRC; this applies the same
// generic per-bit reduction major_sync_crc's table encodes, generalised to a
// partial final byte, which is the standard construction for an
// MSB-first, non-reflected, zero-init CRC of this shape.
[[nodiscard]] constexpr std::uint8_t restart_header_crc(std::span<const std::byte> data,
                                                         std::size_t bit_count) {
    // x^4 + x^3 + x^2 + 1 = 0x10 + 0x8 + 0x4 + 0x1 = 0x1D (x^8 is the
    // implicit register-width term, dropped the same way as major_sync_crc's
    // own polynomial constant).
    constexpr std::uint8_t kPolynomial = 0x1D;
    std::uint8_t crc = 0;
    for (std::size_t i = 0; i < bit_count; ++i) {
        const std::size_t byte_index = i >> 3;
        const int bit_in_byte = 7 - static_cast<int>(i & 7);
        const auto bit = (std::to_integer<std::uint32_t>(data[byte_index]) >> bit_in_byte) & 1;
        const auto msb = (crc >> 7) & 1;
        crc = static_cast<std::uint8_t>(crc << 1);
        if (((msb ^ bit) & 1) != 0) {
            crc ^= kPolynomial;
        }
    }
    return crc;
}

}  // namespace ac3::mlp
