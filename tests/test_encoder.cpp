#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <numbers>
#include <span>
#include <vector>

#include "ac3/core/crc16.hpp"
#include "ac3/core/tables.hpp"
#include "ac3/encoder/encoder.hpp"

namespace {

std::vector<float> sine_frame(std::uint64_t& n, double freq, double amplitude) {
    std::vector<float> samples(ac3::kSamplesPerFrame);
    for (auto& s : samples) {
        s = static_cast<float>(amplitude *
                               std::sin(2.0 * std::numbers::pi * freq * n / 48000.0));
        ++n;
    }
    return samples;
}

void check_frame_invariants(const std::vector<std::byte>& frame, ac3::SampleRate sr,
                            std::uint32_t kbps) {
    CHECK(frame.size() == ac3::frame_size_bytes(sr, kbps).value());
    const std::span<const std::byte> bytes{frame};
    const auto words = static_cast<std::uint32_t>(frame.size()) / 2;
    const std::uint32_t words58 = ac3::frame_size_58_words(words);
    CHECK(ac3::crc16(bytes.subspan(2, 2 * words58 - 2)) == 0x0000);
    CHECK(ac3::crc16(bytes.subspan(2)) == 0x0000);
    CHECK(std::to_integer<std::uint8_t>(bytes[0]) == 0x0B);
    CHECK(std::to_integer<std::uint8_t>(bytes[1]) == 0x77);
}

}  // namespace

TEST_CASE("encoded sine frames satisfy the frame invariants at every bitrate", "[encoder]") {
    for (const std::uint32_t kbps : {96u, 192u, 448u, 640u}) {
        CAPTURE(kbps);
        ac3::StereoEncoder encoder{{.bitrate_kbps = kbps}};
        std::uint64_t n = 0;
        for (int f = 0; f < 3; ++f) {
            const auto samples = sine_frame(n, 1000.0, 0.5);
            const auto frame = encoder.encode_frame(samples, samples);
            REQUIRE(frame.has_value());
            check_frame_invariants(*frame, ac3::SampleRate::k48000, kbps);
        }
    }
}

TEST_CASE("silence through the real encoder produces valid frames", "[encoder]") {
    ac3::StereoEncoder encoder{{.bitrate_kbps = 192}};
    const std::vector<float> silence(ac3::kSamplesPerFrame, 0.0f);
    for (int f = 0; f < 2; ++f) {
        const auto frame = encoder.encode_frame(silence, silence);
        REQUIRE(frame.has_value());
        check_frame_invariants(*frame, ac3::SampleRate::k48000, 192);
    }
}

TEST_CASE("encoding is deterministic", "[encoder]") {
    ac3::StereoEncoder a{{.bitrate_kbps = 256}};
    ac3::StereoEncoder b{{.bitrate_kbps = 256}};
    std::uint64_t n1 = 0;
    std::uint64_t n2 = 0;
    for (int f = 0; f < 2; ++f) {
        const auto samples1 = sine_frame(n1, 3000.0, 0.8);
        const auto samples2 = sine_frame(n2, 3000.0, 0.8);
        const auto frame1 = a.encode_frame(samples1, samples1);
        const auto frame2 = b.encode_frame(samples2, samples2);
        REQUIRE(frame1.has_value());
        REQUIRE(frame2.has_value());
        CHECK(*frame1 == *frame2);
    }
}

TEST_CASE("invalid encoder configs are rejected", "[encoder]") {
    const std::vector<float> silence(ac3::kSamplesPerFrame, 0.0f);
    ac3::StereoEncoder bad_rate{{.bitrate_kbps = 100}};
    CHECK(bad_rate.encode_frame(silence, silence).error() == ac3::FrameError::kInvalidBitrate);
    ac3::StereoEncoder bad_dialnorm{{.bitrate_kbps = 192, .dialnorm = 0}};
    CHECK(bad_dialnorm.encode_frame(silence, silence).error() ==
          ac3::FrameError::kInvalidDialnorm);
}
