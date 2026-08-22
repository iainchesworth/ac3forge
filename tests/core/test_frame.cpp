#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdint>
#include <random>
#include <span>
#include <vector>

#include "ac3/core/crc16.hpp"
#include "ac3/core/tables.hpp"
#include "ac3/encoder/silent_frame.hpp"

namespace {

std::uint8_t u8(std::span<const std::byte> bytes, std::size_t index) {
    return std::to_integer<std::uint8_t>(bytes[index]);
}

}  // namespace

TEST_CASE("gf2 helpers: multiplicative identities", "[crc16][gf2]") {
    STATIC_CHECK(ac3::gf2::mul_mod(1, 1) == 1);
    STATIC_CHECK(ac3::gf2::mul_mod(0x1234, 1) == 0x1234);
    STATIC_CHECK(ac3::gf2::mul_mod(2, ac3::gf2::kInverseX) == 1);
    STATIC_CHECK(ac3::gf2::pow_mod(ac3::gf2::kInverseX, 0) == 1);
    // pow(x, n) * pow(inv_x, n) == 1
    STATIC_CHECK(ac3::gf2::mul_mod(ac3::gf2::pow_mod(2, 123), ac3::gf2::pow_mod(ac3::gf2::kInverseX, 123)) == 1);
}

TEST_CASE("solve_leading_crc zeroes the register over [crc || body]", "[crc16][gf2]") {
    std::mt19937 rng(1536);
    std::uniform_int_distribution<int> byte_dist(0, 255);
    std::uniform_int_distribution<int> len_dist(1, 300);

    for (int trial = 0; trial < 25; ++trial) {
        std::vector<std::byte> body(static_cast<std::size_t>(len_dist(rng)));
        for (auto& b : body) {
            b = static_cast<std::byte>(byte_dist(rng));
        }
        const std::uint16_t crc1 = ac3::solve_leading_crc(body);

        std::vector<std::byte> region;
        region.push_back(static_cast<std::byte>(crc1 >> 8));
        region.push_back(static_cast<std::byte>(crc1 & 0xFF));
        region.insert(region.end(), body.begin(), body.end());
        CHECK(ac3::crc16(region) == 0x0000);
    }
}

TEST_CASE("silent frame has the exact Table 5.18 size", "[frame]") {
    for (const std::uint32_t kbps : {96u, 192u, 448u, 640u}) {
        const auto frame = ac3::build_silent_stereo_frame({.bitrate_kbps = kbps});
        REQUIRE(frame.has_value());
        CHECK(frame->size() == ac3::frame_size_bytes(ac3::SampleRate::k48000, kbps).value());
    }
}

TEST_CASE("silent frame header fields", "[frame]") {
    const auto frame = ac3::build_silent_stereo_frame({.bitrate_kbps = 192});
    REQUIRE(frame.has_value());
    const std::span<const std::byte> bytes{*frame};

    // syncword 0x0B77
    CHECK(u8(bytes, 0) == 0x0B);
    CHECK(u8(bytes, 1) == 0x77);

    // byte 4: fscod (2 bits, 48 kHz = 0) | frmsizecod (6 bits, 192 kbps index
    // 10 -> code 20): 0b00'010100
    CHECK(u8(bytes, 4) == 0x14);

    // byte 5: bsid 8 (5 bits) | bsmod 0 (3 bits): 0b01000'000
    CHECK(u8(bytes, 5) == 0x40);

    // byte 6: acmod 2 (3) | dsurmod 0 (2) | lfeon 0 (1) | dialnorm[4:3]
    // dialnorm=31=0b11111: top two bits here -> 0b010'00'0'11
    CHECK(u8(bytes, 6) == 0x43);
}

TEST_CASE("size, CRCs, and 5.5 constraints across the full config matrix", "[frame]") {
    using ac3::SampleRate;
    for (const auto sr : {SampleRate::k48000, SampleRate::k44100, SampleRate::k32000}) {
        for (const std::uint32_t kbps : ac3::kBitratesKbps) {
            for (const bool pad : {false, true}) {
                if (pad && sr != SampleRate::k44100) {
                    continue;
                }
                CAPTURE(static_cast<int>(sr), kbps, pad);
                const auto frame = ac3::build_silent_stereo_frame(
                    {.sample_rate = sr, .bitrate_kbps = kbps, .pad441 = pad});
                REQUIRE(frame.has_value());
                CHECK(frame->size() == ac3::frame_size_bytes(sr, kbps, pad).value());

                const std::span<const std::byte> bytes{*frame};
                const auto words = static_cast<std::uint32_t>(frame->size()) / 2;
                const std::uint32_t words58 = ac3::frame_size_58_words(words);
                CHECK(words58 == (words >> 1) + (words >> 3));

                // crc1: register reads zero over bytes [2, 2*words58) — sync
                // word excluded, crc1 word leading the region (A/52 7.10.1).
                CHECK(ac3::crc16(bytes.subspan(2, 2 * words58 - 2)) == 0x0000);
                // crc2: register reads zero over the whole frame minus sync.
                CHECK(ac3::crc16(bytes.subspan(2)) == 0x0000);

                // A/52 5.5 bullet 2: aux + errorcheck tail must fit in the
                // final 3/8 of the syncframe (block 5 has no mantissa data).
                const auto plan = ac3::detail::plan_padding(
                    static_cast<std::uint32_t>(frame->size()) * 8 - ac3::detail::kContentBits -
                    ac3::detail::kTailBits);
                const std::uint32_t final38_bits = (words - words58) * 16;
                CHECK(plan.aux_bits + ac3::detail::kTailBits <= final38_bits);

                // A/52 5.5 bullet 1: syncinfo + bsi + blocks 0-1 must fit in
                // the first 5/8 (skip fill is rear-loaded, so blocks 0-1 only
                // carry skip data in the very largest frames).
                std::uint32_t head_bits = ac3::detail::kSyncinfoBsiBits + ac3::detail::kBlock0Bits +
                                          ac3::detail::kReuseBlockBits;
                for (int block = 0; block < 2; ++block) {
                    if (const auto skip = plan.skip_bytes[static_cast<std::size_t>(block)]) {
                        head_bits += 9 + 8u * skip;
                    }
                }
                CHECK(head_bits <= words58 * 16);
            }
        }
    }
}

TEST_CASE("invalid configs are rejected", "[frame]") {
    CHECK(ac3::build_silent_stereo_frame({.bitrate_kbps = 100}).error() ==
          ac3::FrameError::kInvalidBitrate);
    CHECK(ac3::build_silent_stereo_frame({.bitrate_kbps = 192, .dialnorm = 0}).error() ==
          ac3::FrameError::kInvalidDialnorm);
    CHECK(ac3::build_silent_stereo_frame({.bitrate_kbps = 192, .dialnorm = 32}).error() ==
          ac3::FrameError::kInvalidDialnorm);
}
