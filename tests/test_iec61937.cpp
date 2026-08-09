#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <numbers>
#include <optional>
#include <span>
#include <vector>

#include "ac3/decoder/decoder.hpp"
#include "ac3/encoder/eac3_frame.hpp"
#include "ac3/encoder/encoder.hpp"
#include "ac3/encoder/silent_frame.hpp"
#include "ac3/sinks/iec61937.hpp"

// Byte-level regression for the IEC 61937 packers. AC-3's wrap_frame had no
// dedicated test file at all before this one - the CLI/GUI exercised it, but
// nothing pinned the byte layout. Verified against FFmpeg's spdifenc.c and
// (for E-AC-3) Microsoft's own IEC 61937 documentation, both fetched live
// rather than recalled - see docs/HISTORY.md for the sources.

namespace {

std::uint8_t u8(std::span<const std::byte> bytes, std::size_t index) {
    return std::to_integer<std::uint8_t>(bytes[index]);
}

std::uint16_t le16(std::span<const std::byte> bytes, std::size_t index) {
    return static_cast<std::uint16_t>(u8(bytes, index) | (u8(bytes, index + 1) << 8));
}

// Un-swap a burst's payload words back into elementary-stream byte order, the
// inverse of the packers' own word-swap-and-pad, so a round trip can compare
// against the original frame bytes.
std::vector<std::byte> unswap_words(std::span<const std::byte> payload) {
    std::vector<std::byte> out(payload.size());
    for (std::size_t i = 0; i + 1 < payload.size(); i += 2) {
        out[i] = payload[i + 1];
        out[i + 1] = payload[i];
    }
    return out;
}

// A real tone, not silence: §7.2.2.1.1 makes an all-zero-SNR-offset frame
// pure syntax with no mantissas at all, which would validate the header
// layout but nothing about what the packer actually moves.
std::vector<std::vector<float>> tone_frame(int channels, std::uint64_t start) {
    std::vector<std::vector<float>> pcm(
        static_cast<std::size_t>(channels),
        std::vector<float>(static_cast<std::size_t>(ac3::kSamplesPerFrame)));
    for (auto& channel : pcm) {
        for (int i = 0; i < ac3::kSamplesPerFrame; ++i) {
            const auto n = static_cast<double>(start + static_cast<std::uint64_t>(i));
            channel[static_cast<std::size_t>(i)] =
                static_cast<float>(0.5 * std::sin(2.0 * std::numbers::pi * 1000.0 * n / 48000.0));
        }
    }
    return pcm;
}

// A minimal E-AC-3 syncframe header, real enough for Eac3BurstPacker::push to
// read bsid/fscod/numblkscod from (it looks no further), for exercising the
// multi-frame accumulation this project's own encoder never triggers on its
// own (it always emits numblkscod 3 - six blocks - so genuine multi-frame
// accumulation needs a hand-built stream). `payload_words` pads the frame to
// an even, plausible size; the packer does not otherwise inspect the body.
std::vector<std::byte> fake_syncframe(int bsid, int fscod, int numblkscod,
                                      std::size_t payload_words = 4) {
    std::vector<std::byte> frame(6 + payload_words * 2, std::byte{0xAB});
    frame[0] = std::byte{0x0B};
    frame[1] = std::byte{0x77};
    frame[2] = std::byte{0x00};
    frame[3] = std::byte{0x00};
    // fscod(2) | numblkscod(2) | acmod(3)=2 | lfeon(1)=0
    frame[4] = static_cast<std::byte>((fscod << 6) | (numblkscod << 4) | (2 << 1));
    // bsid(5) | dialnorm high 3 bits
    frame[5] = static_cast<std::byte>((bsid << 3) | 0x7);
    return frame;
}

}  // namespace

// Literal, not symbolic: the burst-size constants are the oracle everything
// else in this file trusts, so a test that only compared against
// kBurstBytes/kEac3BurstBytes themselves could not catch either being wrong
// - it would just be comparing the bug to itself.
TEST_CASE("burst sizes match IEC 61937 exactly: AC-3 6144, E-AC-3 24576 (4x)",
         "[iec61937]") {
    CHECK(ac3::iec61937::kBurstBytes == 6144);
    CHECK(ac3::iec61937::kEac3BurstBytes == 24576);
}

// --- AC-3 (wrap_frame) ------------------------------------------------------

TEST_CASE("wrap_frame: preamble and burst size", "[iec61937][ac3]") {
    const auto frame = ac3::build_silent_stereo_frame({.bitrate_kbps = 192});
    REQUIRE(frame.has_value());
    const auto burst = ac3::iec61937::wrap_frame(*frame);
    REQUIRE(burst.has_value());
    CHECK(burst->size() == ac3::iec61937::kBurstBytes);

    // Pa 0xF872, Pb 0x4E1F, little-endian.
    CHECK(le16(*burst, 0) == 0xF872);
    CHECK(le16(*burst, 2) == 0x4E1F);
    // Pc: data type 1 (AC-3) with bsmod (0 for a silent default frame) in
    // bits 8..10.
    CHECK(le16(*burst, 4) == 1);
    // Pd: payload length in BITS, unlike E-AC-3.
    CHECK(le16(*burst, 6) == static_cast<std::uint16_t>(frame->size() * 8));
}

TEST_CASE("wrap_frame: payload is word-swapped and zero-padded", "[iec61937][ac3]") {
    const auto frame = ac3::build_silent_stereo_frame({.bitrate_kbps = 192});
    REQUIRE(frame.has_value());
    const auto burst = ac3::iec61937::wrap_frame(*frame);
    REQUIRE(burst.has_value());

    const std::span<const std::byte> payload{*burst};
    const auto recovered = unswap_words(payload.subspan(8, frame->size()));
    CHECK(std::equal(recovered.begin(), recovered.end(), frame->begin(), frame->end()));

    for (std::size_t i = 8 + frame->size(); i < burst->size(); ++i) {
        CHECK(u8(*burst, i) == 0);
    }
}

TEST_CASE("wrap_frame: round-trips through split_frames", "[iec61937][ac3]") {
    ac3::FrameEncoder encoder{{.bitrate_kbps = 192}};
    std::uint64_t n = 0;
    std::vector<std::byte> last;
    for (int f = 0; f < 3; ++f) {
        auto pcm = tone_frame(2, n);
        n += ac3::kSamplesPerFrame;
        const std::vector<std::span<const float>> views{pcm[0], pcm[1]};
        const auto frame = encoder.encode_frame(views);
        REQUIRE(frame.has_value());
        last = *frame;
    }

    const auto burst = ac3::iec61937::wrap_frame(last);
    REQUIRE(burst.has_value());
    const auto recovered = unswap_words(std::span{*burst}.subspan(8, last.size()));
    const auto frames = ac3::split_frames(recovered);
    REQUIRE(frames.has_value());
    REQUIRE(frames->size() == 1);
    CHECK(std::equal((*frames)[0].begin(), (*frames)[0].end(), last.begin(), last.end()));
}

TEST_CASE("wrap_frame: rejects non-AC-3 input and oversized frames", "[iec61937][ac3]") {
    CHECK(ac3::iec61937::wrap_frame(std::vector<std::byte>{std::byte{0}, std::byte{0}}).error() ==
          ac3::iec61937::WrapError::kNotAFrame);

    // A frame that carries a legal sync word but is too big for one burst.
    std::vector<std::byte> oversized(ac3::iec61937::kBurstBytes, std::byte{0});
    oversized[0] = std::byte{0x0B};
    oversized[1] = std::byte{0x77};
    CHECK(ac3::iec61937::wrap_frame(oversized).error() ==
          ac3::iec61937::WrapError::kFrameTooLarge);
}

// --- E-AC-3 (Eac3BurstPacker) -----------------------------------------------

TEST_CASE("Eac3BurstPacker: real audio, numblkscod 3 bursts every access unit",
         "[iec61937][eac3]") {
    // This project's own encoder always writes numblkscod 3 (six blocks per
    // syncframe - see eac3_frame.cpp), so a real access unit already spans
    // one whole burst period and needs no accumulation.
    ac3::eac3::AccessUnitEncoder encoder{{.independent = {.bitrate_kbps = 192}}};
    ac3::iec61937::Eac3BurstPacker packer;
    std::uint64_t n = 0;
    for (int f = 0; f < 3; ++f) {
        auto pcm = tone_frame(2, n);
        n += ac3::kSamplesPerFrame;
        const std::vector<std::span<const float>> views{pcm[0], pcm[1]};
        const auto unit = encoder.encode_access_unit(views);
        REQUIRE(unit.has_value());

        const auto burst = packer.push(unit->bytes);
        REQUIRE(burst.has_value());
        REQUIRE(burst->has_value());
        const auto& b = **burst;
        CHECK(b.size() == ac3::iec61937::kEac3BurstBytes);
        CHECK(le16(b, 0) == 0xF872);
        CHECK(le16(b, 2) == 0x4E1F);
        // Pc: IEC61937_EAC3 = 0x15, no data-type-dependent bits.
        CHECK(le16(b, 4) == 0x0015);
        // Pd: payload length in BYTES - the detail most likely to be copied
        // wrong from AC-3's bits.
        CHECK(le16(b, 6) == static_cast<std::uint16_t>(unit->bytes.size()));

        const auto recovered = unswap_words(std::span{b}.subspan(8, unit->bytes.size()));
        CHECK(std::equal(recovered.begin(), recovered.end(), unit->bytes.begin(),
                         unit->bytes.end()));
        for (std::size_t i = 8 + unit->bytes.size(); i < b.size(); ++i) {
            CHECK(u8(b, i) == 0);
        }

        // The payload is still a decodable access unit, not merely
        // byte-identical - confirms wrapping did not corrupt anything a
        // decoder actually reads.
        ac3::Eac3Decoder decoder;
        const auto decoded = decoder.decode_access_unit(recovered);
        REQUIRE(decoded.has_value());
        CHECK(decoded->channels.size() == 2);
    }
}

TEST_CASE("Eac3BurstPacker: accumulates sub-six-block access units", "[iec61937][eac3]") {
    struct Case {
        int numblkscod;
        int repeat;
    };
    for (const auto& c : {Case{0, 6}, Case{1, 3}, Case{2, 2}}) {
        CAPTURE(c.numblkscod, c.repeat);
        ac3::iec61937::Eac3BurstPacker packer;
        std::vector<std::byte> expected_payload;
        std::optional<std::vector<std::byte>> burst;
        for (int i = 0; i < c.repeat; ++i) {
            const auto frame = fake_syncframe(/*bsid=*/16, /*fscod=*/0, c.numblkscod);
            const auto result = packer.push(frame);
            REQUIRE(result.has_value());
            expected_payload.insert(expected_payload.end(), frame.begin(), frame.end());
            if (i + 1 < c.repeat) {
                CHECK_FALSE(result->has_value());
            } else {
                REQUIRE(result->has_value());
                burst = **result;
            }
        }
        REQUIRE(burst.has_value());
        CHECK(burst->size() == ac3::iec61937::kEac3BurstBytes);
        CHECK(le16(*burst, 6) == static_cast<std::uint16_t>(expected_payload.size()));
        const auto recovered =
            unswap_words(std::span{*burst}.subspan(8, expected_payload.size()));
        CHECK(std::equal(recovered.begin(), recovered.end(), expected_payload.begin(),
                         expected_payload.end()));
    }
}

TEST_CASE("Eac3BurstPacker: fscod 3 and bsid <= 10 both mean six blocks regardless of the "
         "numblkscod bits",
         "[iec61937][eac3]") {
    // fscod == 3 selects the reduced-sample-rate path, which never transmits
    // numblkscod - it is implicitly always six blocks (Annex E §E2.3.1.3).
    // numblkscod is set to 0 here (which would otherwise mean "wait for five
    // more") to prove it is genuinely ignored, not just usually consistent.
    {
        ac3::iec61937::Eac3BurstPacker packer;
        const auto frame = fake_syncframe(/*bsid=*/16, /*fscod=*/3, /*numblkscod=*/0);
        const auto result = packer.push(frame);
        REQUIRE(result.has_value());
        REQUIRE(result->has_value());
    }
    // bsid <= 10 (not genuine Annex E syntax) gets the same defensive
    // treatment FFmpeg's spdif_header_eac3 gives it.
    {
        ac3::iec61937::Eac3BurstPacker packer;
        const auto frame = fake_syncframe(/*bsid=*/8, /*fscod=*/0, /*numblkscod=*/0);
        const auto result = packer.push(frame);
        REQUIRE(result.has_value());
        REQUIRE(result->has_value());
    }
}

TEST_CASE("Eac3BurstPacker: rejects non-syncframe input and resets on overflow",
         "[iec61937][eac3]") {
    ac3::iec61937::Eac3BurstPacker packer;
    CHECK(packer.push(std::vector<std::byte>{std::byte{0}, std::byte{0}}).error() ==
          ac3::iec61937::WrapError::kNotAFrame);

    ac3::iec61937::Eac3BurstPacker overflow_packer;
    std::vector<std::byte> oversized(ac3::iec61937::kEac3BurstBytes, std::byte{0});
    oversized[0] = std::byte{0x0B};
    oversized[1] = std::byte{0x77};
    oversized[5] = static_cast<std::byte>(16 << 3);  // bsid 16
    CHECK(overflow_packer.push(oversized).error() == ac3::iec61937::WrapError::kFrameTooLarge);

    // The failed push must not leave stale bytes behind for the next one.
    const auto frame = fake_syncframe(/*bsid=*/16, /*fscod=*/0, /*numblkscod=*/3);
    const auto result = overflow_packer.push(frame);
    REQUIRE(result.has_value());
    REQUIRE(result->has_value());
    CHECK(le16(**result, 6) == static_cast<std::uint16_t>(frame.size()));
}
