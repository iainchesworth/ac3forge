#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdint>
#include <vector>

#include "ac3/core/bitwriter.hpp"

namespace {

std::vector<std::uint8_t> to_u8(const std::vector<std::byte>& bytes) {
    std::vector<std::uint8_t> out;
    out.reserve(bytes.size());
    for (auto b : bytes) {
        out.push_back(std::to_integer<std::uint8_t>(b));
    }
    return out;
}

}  // namespace

TEST_CASE("sync word packs MSB-first", "[bitwriter]") {
    ac3::BitWriter w;
    w.put(0x0B77, 16);
    CHECK(to_u8(w.take()) == std::vector<std::uint8_t>{0x0B, 0x77});
}

TEST_CASE("fields crossing byte boundaries", "[bitwriter]") {
    ac3::BitWriter w;
    // 5 + 11 + 8 = 24 bits: 10101 | 11000000111 | 01010101
    w.put(0b10101, 5);
    w.put(0b11000000111, 11);
    w.put(0b01010101, 8);
    CHECK(w.bit_count() == 24);
    CHECK(to_u8(w.take()) == std::vector<std::uint8_t>{0b10101110, 0b00000111, 0b01010101});
}

TEST_CASE("single-bit writes accumulate to bytes", "[bitwriter]") {
    ac3::BitWriter w;
    for (int i = 0; i < 16; ++i) {
        w.put_bit(i % 2 == 0);  // 1010... = 0xAA
    }
    CHECK(to_u8(w.take()) == std::vector<std::uint8_t>{0xAA, 0xAA});
}

TEST_CASE("byte_align zero-pads a partial byte", "[bitwriter]") {
    ac3::BitWriter w;
    w.put(0b101, 3);
    w.byte_align();
    CHECK(w.bit_count() == 8);
    CHECK(to_u8(w.take()) == std::vector<std::uint8_t>{0b10100000});
}

TEST_CASE("32-bit values are accepted", "[bitwriter]") {
    ac3::BitWriter w;
    w.put(0xDEADBEEF, 32);
    CHECK(to_u8(w.take()) == std::vector<std::uint8_t>{0xDE, 0xAD, 0xBE, 0xEF});
}

TEST_CASE("patch_u16 overwrites emitted output in place", "[bitwriter]") {
    ac3::BitWriter w;
    w.put(0x0B77, 16);
    w.put(0x0000, 16);  // crc1 placeholder at byte offset 2
    w.put(0x1234, 16);
    w.patch_u16(2, 0xBEEF);
    CHECK(to_u8(w.take()) == std::vector<std::uint8_t>{0x0B, 0x77, 0xBE, 0xEF, 0x12, 0x34});
}

TEST_CASE("zero-width writes are a no-op", "[bitwriter]") {
    ac3::BitWriter w;
    w.put(0, 0);
    CHECK(w.bit_count() == 0);
    w.put(0x7, 3);
    w.put(0, 0);
    CHECK(w.bit_count() == 3);
}
