#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdint>
#include <span>

#include "ac3/core/crc16.hpp"
#include "ac3/encoder/eac3_frame.hpp"

namespace {

std::uint8_t u8(std::span<const std::byte> bytes, std::size_t index) {
    return std::to_integer<std::uint8_t>(bytes[index]);
}

}  // namespace

TEST_CASE("E-AC-3 frame size follows the bit rate exactly", "[eac3]") {
    // frmsiz signals the size directly, so there is no table and no 44.1 kHz
    // padding alternation: the frame is simply the exact bit budget in words.
    STATIC_CHECK(ac3::eac3::frame_words(ac3::SampleRate::k48000, 192) == 384);
    STATIC_CHECK(ac3::eac3::frame_words(ac3::SampleRate::k48000, 640) == 1280);
    STATIC_CHECK(ac3::eac3::frame_words(ac3::SampleRate::k32000, 192) == 576);
}

TEST_CASE("E-AC-3 header fields", "[eac3]") {
    const auto frame = ac3::eac3::build_silent_frame({.bitrate_kbps = 192});
    REQUIRE(frame.has_value());
    const std::span<const std::byte> bytes{*frame};
    CHECK(frame->size() == 768);

    // Sync word, then bsi begins immediately - E-AC-3 has no crc1.
    CHECK(u8(bytes, 0) == 0x0B);
    CHECK(u8(bytes, 1) == 0x77);

    // byte 2: strmtyp(2)=0 | substreamid(3)=0 | frmsiz(11) high 3 bits.
    // frmsiz = words - 1 = 383 = 0b00101111111, so the top 3 bits are 001.
    CHECK(u8(bytes, 2) == 0x01);
    // byte 3: the remaining 8 bits of frmsiz = 0b01111111.
    CHECK(u8(bytes, 3) == 0x7F);
    // byte 4: fscod(2)=0 | numblkscod(2)=3 | acmod(3)=2 | lfeon(1)=0
    //         = 00 11 010 0
    CHECK(u8(bytes, 4) == 0x34);
    // byte 5: bsid(5)=16 | dialnorm high 3 bits (31 = 11111) = 10000 111
    CHECK(u8(bytes, 5) == 0x87);
}

TEST_CASE("E-AC-3 crc2 covers the frame after the sync word", "[eac3]") {
    for (const std::uint32_t kbps : {96u, 192u, 448u, 640u}) {
        CAPTURE(kbps);
        const auto frame = ac3::eac3::build_silent_frame({.bitrate_kbps = kbps});
        REQUIRE(frame.has_value());
        const std::span<const std::byte> bytes{*frame};
        // There is no crc1 in E-AC-3, so the only check is the trailing one:
        // the register must read zero over everything past the sync word.
        CHECK(ac3::crc16(bytes.subspan(2)) == 0x0000);
    }
}

TEST_CASE("E-AC-3 layouts and rates produce correctly sized frames", "[eac3]") {
    using ac3::Acmod;
    for (const auto acmod : {Acmod::k1_0, Acmod::k2_0, Acmod::k3_2}) {
        for (const bool lfe : {false, true}) {
            for (const std::uint32_t kbps : {96u, 192u, 448u}) {
                CAPTURE(static_cast<int>(acmod), lfe, kbps);
                const auto frame = ac3::eac3::build_silent_frame(
                    {.bitrate_kbps = kbps, .acmod = acmod, .lfe = lfe});
                if (!frame) {
                    // Legitimate: a wide layout at a low rate cannot fit even
                    // its own exponent sets. The contract is a clean refusal,
                    // never an undersized frame.
                    CHECK(frame.error() == ac3::FrameError::kInvalidBitrate);
                    continue;
                }
                CHECK(frame->size() ==
                      ac3::eac3::frame_words(ac3::SampleRate::k48000, kbps) * 2);
                CHECK(ac3::crc16(std::span{*frame}.subspan(2)) == 0x0000);
            }
        }
    }
}

TEST_CASE("E-AC-3 rejects configurations it cannot express", "[eac3]") {
    CHECK(ac3::eac3::build_silent_frame({.bitrate_kbps = 100}).error() ==
          ac3::FrameError::kInvalidBitrate);
    CHECK(ac3::eac3::build_silent_frame({.bitrate_kbps = 192, .dialnorm = 0}).error() ==
          ac3::FrameError::kInvalidDialnorm);
    // 1+1 needs a second program's metadata throughout; not supported yet.
    CHECK(ac3::eac3::build_silent_frame(
              {.bitrate_kbps = 192, .acmod = ac3::Acmod::kDualMono})
              .has_value() == false);
}
