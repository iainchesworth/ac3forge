#include "ac3/mlp/extra_data.hpp"

#include <cassert>
#include <cstdint>

#include "ac3/mlp/mlp_tables.hpp"

namespace ac3::mlp {

namespace {

// §4.8.5: the XOR of every byte in the expansion excluding the length word
// and the parity byte itself, XORed with 0xA9 "to force the check to fail
// in the event of the stream consisting entirely of zeroes".
[[nodiscard]] std::uint8_t extra_data_parity(std::span<const std::byte> data_and_padding) {
    std::uint8_t parity = kParityXorConstant;
    for (const auto b : data_and_padding) {
        parity = static_cast<std::uint8_t>(parity ^ std::to_integer<std::uint8_t>(b));
    }
    return parity;
}

}  // namespace

std::vector<std::byte> build_extra_data(std::span<const std::byte> payload) {
    if (payload.empty()) {
        return {};
    }
    // Total = 2 length bytes + payload + padding + 1 parity byte, rounded
    // to 16-bit words; §4.8.3's field holds one less than the word count.
    const std::size_t total_words = (2 + payload.size() + 1 + 1) / 2;
    assert(total_words - 1 < 4096);
    const std::size_t padding = total_words * 2 - 2 - payload.size() - 1;
    assert(padding <= 1);

    const auto length_value = static_cast<std::uint16_t>(total_words - 1);
    // §4.8.2: the XOR of the check nibble and the length's three nibbles
    // is 0xF.
    std::uint8_t known = 0;
    for (int shift = 8; shift >= 0; shift -= 4) {
        known ^= static_cast<std::uint8_t>((length_value >> shift) & 0xF);
    }
    const auto nibble = static_cast<std::uint8_t>(0xF ^ known);

    std::vector<std::byte> out;
    out.reserve(total_words * 2);
    out.push_back(static_cast<std::byte>((nibble << 4) | ((length_value >> 8) & 0xF)));
    out.push_back(static_cast<std::byte>(length_value & 0xFF));
    out.insert(out.end(), payload.begin(), payload.end());
    for (std::size_t i = 0; i < padding; ++i) {
        out.push_back(std::byte{0});
    }
    out.push_back(static_cast<std::byte>(
        extra_data_parity(std::span<const std::byte>{out}.subspan(2))));
    assert(out.size() == total_words * 2);
    return out;
}

bool parse_extra_data(std::span<const std::byte> data, std::vector<std::byte>& payload) {
    payload.clear();
    if (data.size() < 2 || data.size() % 2 != 0) {
        return false;
    }
    // §4.8: a first word of zero means the area is nothing but padding and
    // may be discarded.
    if (std::to_integer<std::uint8_t>(data[0]) == 0 &&
        std::to_integer<std::uint8_t>(data[1]) == 0) {
        return true;
    }
    const auto first = std::to_integer<std::uint8_t>(data[0]);
    const auto nibble = static_cast<std::uint8_t>(first >> 4);
    const auto length_value = static_cast<std::uint16_t>(
        ((first & 0xF) << 8) | std::to_integer<std::uint8_t>(data[1]));
    std::uint8_t check = nibble;
    for (int shift = 8; shift >= 0; shift -= 4) {
        check ^= static_cast<std::uint8_t>((length_value >> shift) & 0xF);
    }
    if (check != 0xF) {
        return false;
    }
    const std::size_t total = (static_cast<std::size_t>(length_value) + 1) * 2;
    if (total != data.size() || total < 4) {
        return false;
    }
    const auto covered = data.subspan(2, total - 3);  // data() + padding
    if (extra_data_parity(covered) !=
        std::to_integer<std::uint8_t>(data[total - 1])) {
        return false;
    }
    payload.assign(covered.begin(), covered.end());
    return true;
}

}  // namespace ac3::mlp
