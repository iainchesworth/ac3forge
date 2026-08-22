#include "ac3/sinks/iec61937.hpp"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <utility>
#include <vector>

#include "ac3/core/eac3_tables.hpp"

namespace ac3::iec61937 {

namespace {

void put_word_le(std::vector<std::byte>& out, std::uint16_t word) {
    out.push_back(static_cast<std::byte>(word & 0xFF));
    out.push_back(static_cast<std::byte>(word >> 8));
}

// Shared by wrap_frame and Eac3BurstPacker: the elementary stream is
// big-endian within each 16-bit word, the IEC 61937 carrier is little-endian
// PCM16, and the burst is zero-padded to its fixed length either way. Both
// AC-3 and E-AC-3 syncframes are measured in whole 16-bit words (Table 5.18's
// byte counts and Annex E's frmsiz are both word counts), so payload is
// always even here — nothing legal reaches the odd-trailing-byte case IEC
// 61937 otherwise has to handle.
void pack_payload_words(std::vector<std::byte>& burst, std::span<const std::byte> payload,
                        std::size_t burst_bytes) {
    for (std::size_t i = 0; i + 1 < payload.size(); i += 2) {
        put_word_le(burst, static_cast<std::uint16_t>(
                               (std::to_integer<std::uint16_t>(payload[i]) << 8) |
                               std::to_integer<std::uint16_t>(payload[i + 1])));
    }
    burst.resize(burst_bytes, std::byte{0});
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
    pack_payload_words(burst, frame, kBurstBytes);
    return burst;
}

std::expected<std::optional<std::vector<std::byte>>, WrapError> Eac3BurstPacker::push(
    std::span<const std::byte> access_unit) {
    if (access_unit.size() < 6 || (access_unit.size() & 1) != 0 ||
        std::to_integer<std::uint8_t>(access_unit[0]) != 0x0B ||
        std::to_integer<std::uint8_t>(access_unit[1]) != 0x77) {
        return std::unexpected(WrapError::kNotAFrame);
    }
    if (pending_.size() + access_unit.size() + 8 > kEac3BurstBytes) {
        pending_.clear();
        blocks_pending_ = 0;
        return std::unexpected(WrapError::kFrameTooLarge);
    }

    // byte 4: fscod (2 bits) | numblkscod (2 bits) | acmod (3 bits) | lfeon
    // (1 bit). byte 5's top 5 bits are bsid. Same layout this project's own
    // eac3_decoder.cpp parses, read directly here the way spdif_header_eac3
    // does (pkt->data[4], pkt->data[5]) rather than pulling in a decoder.
    const auto byte4 = std::to_integer<std::uint32_t>(access_unit[4]);
    const auto bsid = std::to_integer<std::uint32_t>(access_unit[5]) >> 3;
    const auto fscod = byte4 >> 6;
    const auto numblkscod = (byte4 >> 4) & 0x3;
    // fscod == 3 selects the reduced-sample-rate path (§E2.3.1.3), which does
    // not transmit numblkscod at all — it is implicitly always the six-block
    // code. Mirrors spdif_header_eac3's own bsid > 10 && fscod != 3 guard.
    const int blocks = (bsid > 10 && fscod != 3)
                           ? eac3::blocks_per_syncframe(static_cast<int>(numblkscod))
                           : 6;

    pending_.insert(pending_.end(), access_unit.begin(), access_unit.end());
    blocks_pending_ += blocks;
    if (blocks_pending_ < 6) {
        return std::nullopt;
    }

    std::vector<std::byte> burst;
    burst.reserve(kEac3BurstBytes);
    put_word_le(burst, 0xF872);  // Pa
    put_word_le(burst, 0x4E1F);  // Pb
    put_word_le(burst, 0x0015);  // Pc: IEC61937_EAC3, no data-type-dependent bits
    put_word_le(burst, static_cast<std::uint16_t>(pending_.size()));  // Pd: BYTES, not bits
    pack_payload_words(burst, pending_, kEac3BurstBytes);

    pending_.clear();
    blocks_pending_ = 0;
    return std::optional<std::vector<std::byte>>{std::move(burst)};
}

std::expected<std::vector<std::byte>, WrapError> wrap_stream(
    std::span<const std::span<const std::byte>> units, bool eac3) {
    std::vector<std::byte> payload;
    if (eac3) {
        Eac3BurstPacker packer;
        for (const auto& unit : units) {
            const auto burst = packer.push(unit);
            if (!burst) {
                return std::unexpected(burst.error());
            }
            if (*burst) {
                payload.insert(payload.end(), (**burst).begin(), (**burst).end());
            }
        }
        return payload;
    }
    payload.reserve(units.size() * kBurstBytes);
    for (const auto& unit : units) {
        const auto burst = wrap_frame(unit);
        if (!burst) {
            return std::unexpected(burst.error());
        }
        payload.insert(payload.end(), burst->begin(), burst->end());
    }
    return payload;
}

}  // namespace ac3::iec61937
