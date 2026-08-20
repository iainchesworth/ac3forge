#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdint>
#include <random>
#include <vector>

#include "ac3/core/crc16.hpp"

namespace {

std::vector<std::byte> to_bytes(const char* text) {
    std::vector<std::byte> out;
    for (; *text != '\0'; ++text) {
        out.push_back(static_cast<std::byte>(*text));
    }
    return out;
}

}  // namespace

TEST_CASE("known check value (CRC-16/UMTS)", "[crc16]") {
    // Standard catalogue check string for poly 0x8005, init 0, unreflected.
    const auto msg = to_bytes("123456789");
    CHECK(ac3::crc16(msg) == 0xFEE8);
}

TEST_CASE("empty input yields the initial register", "[crc16]") {
    CHECK(ac3::crc16({}) == 0x0000);
    CHECK(ac3::crc16({}, 0xABCD) == 0xABCD);
}

TEST_CASE("crc16 is usable at compile time", "[crc16]") {
    constexpr std::array<std::byte, 2> data{std::byte{0x0B}, std::byte{0x77}};
    constexpr auto value = ac3::crc16(data);
    STATIC_CHECK(value == ac3::crc16(data));
}

TEST_CASE("appending the CRC drives the register to zero", "[crc16]") {
    // This is the property the AC-3 frame CRCs rely on (A/52 §7.10.1): a
    // covered region followed by its CRC word shifts through to zero.
    std::mt19937 rng(0x0B77);
    std::uniform_int_distribution<int> byte_dist(0, 255);
    std::uniform_int_distribution<int> len_dist(1, 512);

    for (int trial = 0; trial < 50; ++trial) {
        std::vector<std::byte> msg(static_cast<std::size_t>(len_dist(rng)));
        for (auto& b : msg) {
            b = static_cast<std::byte>(byte_dist(rng));
        }
        const std::uint16_t crc = ac3::crc16(msg);
        auto with_crc = msg;
        with_crc.push_back(static_cast<std::byte>(crc >> 8));
        with_crc.push_back(static_cast<std::byte>(crc & 0xFF));
        CHECK(ac3::crc16(with_crc) == 0x0000);
    }
}

TEST_CASE("incremental computation matches one-shot", "[crc16]") {
    const auto msg = to_bytes("ac3forge incremental crc check");
    const std::span<const std::byte> all{msg};
    const auto split = msg.size() / 2;
    const std::uint16_t incremental = ac3::crc16(all.subspan(split), ac3::crc16(all.first(split)));
    CHECK(incremental == ac3::crc16(all));
}
